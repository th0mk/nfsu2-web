/* Most Wanted's career bot (TRACKER M4-15; test only, like the race aids of career_mw.c, which it uses).  It plays
 * the career forward the way a player does, through the game's own paths: it reads the Blacklist's gates the way
 * the Blacklist screen reads them, engages the current bin's races at their own start (the engage trigger is at the
 * race's start grid), lets the game's AI drive them (career_mw.c's autopilot, with its wedge reset), answers the
 * screens that come, and earns bounty and milestones in pursuits the game runs.  It never writes progression
 * (L-022): every count it waits for is the game's.
 *
 *   bot gates       log the current bin's gates and its races (below)
 *   bot run N       play until the career's bin is N (or lower), then stop (`bot off` stops it at once);
 *                   bot run N races stops once the bin's races are won (a point to save at), bot run N races K once
 *                   K of them are, bot run N ready once the rival's requirements are met (before his challenge);
 *                   bot run N keep races the current car (else pick_car's)
 *   bot race I      engage race I of the bin's list and race it, then stop (a probe)
 *   bot pursuit     one pursuit in free roam, then stop (a probe)
 *   bot chase S     pursuits one after another (the AI driving, a new one forced 5 s after the last one's end) until
 *                   they have lasted S game seconds in all, then stop (a long pursuit at the heat the career allows:
 *                   the heat stays on the car between pursuits; game over stops it, as everywhere)
 *   bot bust N [pay] pursuits with the car standing (the AI does not drive: the cops bust it) until N busts, then stop:
 *                   a probe of the impound path (the strikes, an Out of Jail marker at the sheet, Car_Select.fng after
 *                   an impound, game over); pay: the fines paid at every sheet, no marker spent
 *   bot car         take the career's next car that can be driven at the safehouse (Jump to Safe House, Car Select,
 *                   Resume Free Roam), then stop: a probe of what decide does after two lost rival challenges
 *   bot save     save at the safehouse: the pause menu's Jump to Safe House, then the safehouse menu's Save
 *   bot wait [N]    (career_mw.c) wait up to N frames for the bot to stop
 *
 * The car it races is the career's best (car_rank: the Car Lot's tier, then the package levels on it, then the
 * latest won), taken in the safehouse's Car Select.  Its order in a bin (decide): performance packages for that car
 * from a customization shop with the cash the career has (once a bin, and again when the cash has grown: shop_frame;
 * the packages that make the car faster first); the bin's races not won yet, the one tried least first (a lost
 * race is raced again in its turn), until the races the rival asks are won; then pursuits until the milestones and
 * the bounty are there; then the rival's challenge, engaged at its first race's start (InGameRivalChallenge.fng,
 * whose Enter accepts it); a lost challenge is tried again, after the shop when the cash reaches a package, and after
 * two lost in one car in the other of the two best cars (B_CAR: Jump to Safe House, Car Select).  Cops on the car in free roam (a pursuit that began in a race goes on after it) are a
 * pursuit first, whatever it was doing.  In a pursuit the AI drives, and a long chase or a boxed-in car is broken off
 * by setting the car far away (pursuit_frame: what was tried besides, and dropped).  A bust adds a strike to the car,
 * and the strike that reaches the car's most impounds it: the bot spends an Out of Jail marker instead when it holds
 * one, and after an impound takes another car that can be driven in Car_Select.fng; with no car left (and no Impound
 * Release marker) the career is over (GameOver.fng): the bot stops there, and on any career menu it does not know.  The beaten rival's markers are taken as they come (two cards, face down); what they gave
 * is logged from the cash and the cars before and after.
 *
 * The career's state, from speed.exe v1.3's code:
 *   the user profile [[0x91CF90]+0x10]: +0xB0 (byte) the current Blacklist bin (16 the prologue, 15 Sonny's),
 *     +0x414 the car table (FEPlayerCarDB), whose 0x590500 (thiscall) is the career's bounty (Game.GetPlayerBounty
 *     0x612300 ends there).
 *   GRaceDatabase [0x91E004]: 0x5E3980 (thiscall, bin number) gives the bin's GRaceBin; its score table: the count at
 *     +0, the entries at +0x3C, 16 bytes each {race hash, flags, ...}, flags & 2 = the race completed (the race
 *     sheet's 0x5DCA70(hash, 2), which also adds a missing hash to the table: the bot reads the table instead).
 *   GRaceBin (an Attrib instance of race_bin_NN): +0x18 (u16) the milestones ("challenges") passed, +0x1A (u16) the
 *     races won (Bin.GetNumChallengesPassed 0x604A10 -> 0x5DD1C0, Bin.GetNumRacesWon 0x604A30 -> 0x5DD1D0); what the
 *     rival asks, read from race_bin_NN by 0x5DD1A0 (races, field 0xD617FEDC), 0x5DD180 (milestones, 0x6DD4B98B)
 *     and 0x5DD160 (bounty, 0xD3657D92): the Blacklist screen's own calls (0x52EE50: SafeHouseReputationOverview);
 *     race_bin_15 asks 3 races, 3 milestones and a bounty of 20,000.  Its races: 0x5DCF70 counts the list (field
 *     0xA7EF40EF), 0x601060(i) gives race i's hash (the race sheet's walk, 0x547E55).
 *   GRaceParameters, 0x5FB710 (thiscall on the database, hash): 0x5FAD10(out) its start position (the start grid's
 *     position field 0x9F743A0E, as simulation space), 0x5FAE00(out) its start direction (from the heading,
 *     0x5A6A57C6).  The race's engage trigger stands there: the car set there and stopped, Enter opens
 *     EngageEventDialog.fng (Decline highlighted, Accept by LEFT), and Accept loads the race.
 *
 * The screens the bot answers (each Present, mw_bot_present): the winner's InGamePhotoMaster.fng, the loser's
 * PostRace_Results.fng, PostRace_MilestoneRewards.fng and PostRace_Pursuit.fng after a pursuit and the loading
 * screen's "Press Enter" with Enter; a text message (SMS_Message.fng) and the mailbox under it with Escape (Enter does
 * not close them); EngageEventDialog.fng with LEFT then Enter when the bot engaged it (else Enter: Decline); the
 * safehouse menu, the pause menu and their dialogs as the state wants (below); the infractions sheet after a bust
 * (Infractions.fng) with Enter, which pays the fines and takes the strike (or the Out of Jail marker, above); the car
 * list after it (Car_Select.fng) with a car that can be driven; any other screen that takes input is logged and
 * answered with Enter after a pause, a few times, then left (the log says so). */
#include "cpu.h"
#include "game.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

long autotest_frame(void);
void autotest_tap(unsigned dik, long frames);
const char *mw_fe_input_top(uint32_t *focus);                 /* fe_mw.c */
uint32_t mw_aid_call(CPU *c, uint32_t fn, uint32_t self, int n, const uint32_t *args);   /* career_mw.c */
uint32_t mw_aid_simable(CPU *c);
uint32_t mw_aid_pursuit(CPU *c, uint32_t *perp);
int mw_aid_where(CPU *c, float pos[3], float *kmh);
int mw_aid_teleport(CPU *c, const float pos[3], const float dir[3]);
void mw_aid_autopilot(CPU *c, int mode);
int mw_reset_car(CPU *c);
int mw_aid_last_end(void);                    /* career_mw.c: 1 busted, 2 evaded since the last read */
void mw_aid_rapsheet(CPU *c, const char *why); /* pursuit_mw.c */
void mw_aid_icons(CPU *c, int type);
void mw_aid_pools(CPU *c, int watch);         /* pursuit_mw.c */
void mw_aid_copcars(const char *why);         /* pursuit_mw.c: the cop cars asked for by name */
double mw_game_time(void);

#define G_PROFILES       0x0091cf90u     /* [+0x10] the user profile */
#define PROF_BIN         0xb0u
#define PROF_CASH        0xb4u           /* CareerSettings (+0xA8) +0xC: the cash the safehouse menu shows */
#define PROF_CARDB       0x414u
#define G_RACEDB         0x0091e004u
#define G_RACESTATUS     0x0091e000u
#define RS_ACTIVE        0x198au
#define RS_RACERS        0x18u           /* GRacerInfo, 0x194 bytes each; +0x21 finished */
#define RACER_SIZE       0x194u
#define RI_FINISHED      0x21u
#define FN_RACER_INDEX   0x0060dae0u     /* Game.GetRacerIndex(simable): 1-based, -1 none */
#define DB_SCORES        0x3cu
#define FN_DB_BIN        0x005e3980u
#define FN_BIN_RACES     0x005dcf70u
#define FN_BIN_RACE      0x00601060u
#define FN_BIN_BOSSES    0x005dcf60u     /* the rival's races: their count (field 0xD5A174AA) */
#define FN_BIN_BOSS      0x00601020u     /* and race i's hash */
#define F_BIN_RACE_LIST  0xa7ef40efu     /* race_bin_NN's races (the collections' keys) */
#define F_BIN_BOSS_LIST  0xd5a174aau     /* and the rival's */
#define FN_BIN_WON       0x005dd1d0u
#define FN_BIN_WON_REQ   0x005dd1a0u
#define FN_BIN_MS        0x005dd1c0u
#define FN_BIN_MS_REQ    0x005dd180u
#define FN_BIN_BOUNTY_REQ 0x005dd160u
#define FN_BOUNTY        0x00590500u
#define FN_DB_PARAMS     0x005fb710u
#define FN_START_POS     0x005fad10u
#define FN_START_DIR     0x005fae00u
#define FN_FORCE_PURSUIT 0x0060aac0u     /* Game.ForcePursuitStart(min heat) */
#define PERP_HEAT        0x1cu
#define PURSUIT_COPS     0x20u
#define PURSUIT_TIME     0xb4u           /* the chase's length (s): it stops while the cops search */
#define PURSUIT_BUST_T   0xdcu           /* the bust timer (s; 5 = busted) */
#define PURSUIT_SEARCH   0x118u          /* the cops' search position (simulation space; GAME 13.9, by dump) */
#define PURSUIT_STATE    0x218u          /* 0 the chase, 2 the cops' search (the cooldown, +0xFC filling to 1), 3 busted;
                                            1 a chase too (one cop on the car for 100 s, the bust timer rising in it) */
#define DIK_ESCAPE 0x01u
#define DIK_RETURN 0x1cu
#define DIK_LEFT   0xcbu
#define MAX_RACES 32

static uint32_t rd(uint32_t a){ return a && mem_is_committed(a) && mem_is_committed(a + 3u) ? MR32(a) : 0u; }
static float rdf(uint32_t a){ uint32_t v = rd(a); float f; memcpy(&f, &v, 4); return f; }
static uint32_t call(CPU *c, uint32_t fn, uint32_t self, int n, const uint32_t *args){ return mw_aid_call(c, fn, self, n, args); }
static uint32_t call1(CPU *c, uint32_t fn, uint32_t self, uint32_t a){ return mw_aid_call(c, fn, self, 1, &a); }

/* ---- the career's state ---- */
struct gates { int bin; uint32_t binobj; int won, won_req, ms, ms_req, bounty, bounty_req, cash, cars; };
static uint32_t profile(void){ return rd(rd(G_PROFILES) + 0x10u); }
/* the career's cars: FEPlayerCarDB's records (200 of 0x14 bytes, +0 the handle, -1 unused) with a career record (byte
 * +0x11 not 0xFF; GAME 13.8), as Car Select in the safehouse counts them ('1 / 2') */
static int career_cars(uint32_t prof){
    int n = 0;
    for (uint32_t i = 0, r = prof + PROF_CARDB; prof && i < 200u; i++, r += 0x14u)
        if (mem_is_committed(r + 0x13u) && MR32(r) != 0xffffffffu && MR8(r + 0x11u) != 0xffu) n++;
    return n;
}
/* a car's career record (FEPlayerCarDB +0x8728, 0x38 bytes each, the car record's byte +0x11 its index; GAME 13.7):
 * +2 the most strikes the car takes, +3 its strikes, +4 0 when it can be driven (2 once impounded: 0x5811A0 counts
 * the cars with it 0); 0 when the car has none */
#define CARDB_CAREER     0x8728u
#define CAREER_SIZE      0x38u
static uint32_t career_rec(uint32_t prof, uint32_t handle){
    for (uint32_t i = 0, r = prof + PROF_CARDB; prof && i < 200u; i++, r += 0x14u)
        if (mem_is_committed(r + 0x13u) && MR32(r) == handle && MR8(r + 0x11u) <= 0x19u){
            uint32_t cr = prof + PROF_CARDB + CARDB_CAREER + CAREER_SIZE * MR8(r + 0x11u);
            return mem_is_committed(cr + CAREER_SIZE - 1u) ? cr : 0u;
        }
    return 0;
}
static int car_free(uint32_t prof, uint32_t handle){ uint32_t cr = career_rec(prof, handle); return cr && !MR8(cr + 4u); }
/* the performance packages installed on a car: its customization record (FEPlayerCarDB +0xFA0, 0x198 bytes each, the
 * car record's byte +0x10 its index) holds the levels as seven dwords at +0x118 (installed_level below); their sum,
 * -1 when the car has no such record */
static int car_levels(uint32_t prof, uint32_t handle){
    for (uint32_t i = 0, r = prof + PROF_CARDB; prof && i < 200u; i++, r += 0x14u)
        if (mem_is_committed(r + 0x13u) && MR32(r) == handle && MR8(r + 0x10u) < 0x4bu){
            uint32_t cz = prof + PROF_CARDB + 0xfa0u + MR8(r + 0x10u) * 0x198u + 0x118u;
            int s = 0;
            for (uint32_t k = 0; k < 7u; k++){ int v = (int)rd(cz + 4u * k); s += v > 0 && v < 8 ? v : 0; }
            return s;
        }
    return -1;
}
/* the markers (0x91D928, 63 of {type, param, state}): 0x576F10 (thiscall (type, param)) counts those held; 0x11 is
 * out_of_jail_free (pursuit_mw.c says more) */
#define G_MARKERS        0x0091d928u
#define FN_MARKERS       0x00576f10u
#define MK_OUT_OF_JAIL   0x11u
/* the markers held, counted by type (the entries 12 bytes each: +0 type, +4 param, +8 state, 1 = held, as 0x576F10
 * reads them), into cnt[32]; the number of entries held */
#define MK_TYPES 32
static int markers_held(int cnt[MK_TYPES]){
    int n = 0;
    memset(cnt, 0, MK_TYPES * sizeof cnt[0]);
    for (uint32_t i = 0, e = G_MARKERS; i < 63u; i++, e += 12u)
        if (rd(e + 8u) == 1u){ uint32_t t = rd(e); cnt[t < MK_TYPES ? t : 0]++; n++; }
    return n;
}
/* the types' names (speed.exe's table at 0x8F5480: {"backroom", name, type} for the parts, 1..0x10) */
static const char *marker_name(int t){
    static const char *const parts[17] = { "type", "brakes", "engine", "nos", "induction", "chassis", "tires", "transmission",
        "bodykit", "hood", "spoiler", "rims", "roofscoop", "hud", "vinyls", "decals", "paint" };
    if (t >= 1 && t <= 0x10) return parts[t];
    switch (t){
    case 0x11: return "out_of_jail_free"; case 0x12: return "pink_slip"; case 0x13: return "cash_bonus";
    case 0x14: return "add_impound_box"; case 0x15: return "release_car_from_impound"; default: return "type";
    }
}
static int read_gates(CPU *c, struct gates *g){
    memset(g, 0, sizeof *g);
    uint32_t prof = profile(), db = rd(G_RACEDB);
    if (!prof || !db) return 0;
    g->bin = MR8(prof + PROF_BIN);
    g->binobj = call1(c, FN_DB_BIN, db, (uint32_t)g->bin);
    g->bounty = (int)call(c, FN_BOUNTY, prof + PROF_CARDB, 0, 0);
    g->cash = (int)rd(prof + PROF_CASH);
    g->cars = career_cars(prof);
    if (!g->binobj) return 1;
    g->won = (int)(call(c, FN_BIN_WON, g->binobj, 0, 0) & 0xffffu);
    g->ms = (int)(call(c, FN_BIN_MS, g->binobj, 0, 0) & 0xffffu);
    g->won_req = (int)call(c, FN_BIN_WON_REQ, g->binobj, 0, 0);
    g->ms_req = (int)call(c, FN_BIN_MS_REQ, g->binobj, 0, 0);
    g->bounty_req = (int)call(c, FN_BIN_BOUNTY_REQ, g->binobj, 0, 0);
    return 1;
}
static uint32_t score_flags(uint32_t hash){
    uint32_t db = rd(G_RACEDB), n = rd(db), e = rd(db + DB_SCORES);
    for (uint32_t i = 0; e && i < n && i < 8192u; i++){
        uint32_t h = rd(e + 16u * i);
        if (!h) break;
        if (h == hash) return rd(e + 16u * i + 4u);
    }
    return 0;
}
/* Attrib::StringHash32 (Jenkins' lookup2, initial value 0xABCDEF00; the game's 0x454830): a collection's key from
 * its full name, h32("default") == 0xEEC2271A (tools/mw/census.py's h32) */
#define MIX(a, b, c) do { \
    a -= b; a -= c; a ^= c >> 13;  b -= c; b -= a; b ^= a << 8;  c -= a; c -= b; c ^= b >> 13; \
    a -= b; a -= c; a ^= c >> 12;  b -= c; b -= a; b ^= a << 16; c -= a; c -= b; c ^= b >> 5; \
    a -= b; a -= c; a ^= c >> 3;   b -= c; b -= a; b ^= a << 10; c -= a; c -= b; c ^= b >> 15; } while (0)
static uint32_t h32(const char *s){
    const uint8_t *k = (const uint8_t *)s; uint32_t n = (uint32_t)strlen(s), len = n, a = 0x9e3779b9u, b = a, c = 0xabcdef00u;
    while (len >= 12){
        a += k[0] | (uint32_t)k[1] << 8 | (uint32_t)k[2] << 16 | (uint32_t)k[3] << 24;
        b += k[4] | (uint32_t)k[5] << 8 | (uint32_t)k[6] << 16 | (uint32_t)k[7] << 24;
        c += k[8] | (uint32_t)k[9] << 8 | (uint32_t)k[10] << 16 | (uint32_t)k[11] << 24;
        MIX(a, b, c); k += 12; len -= 12;
    }
    c += n;
    switch (len){
    case 11: c += (uint32_t)k[10] << 24; /* fall through */
    case 10: c += (uint32_t)k[9] << 16;  /* fall through */
    case 9:  c += (uint32_t)k[8] << 8;   /* fall through */
    case 8:  b += (uint32_t)k[7] << 24;  /* fall through */
    case 7:  b += (uint32_t)k[6] << 16;  /* fall through */
    case 6:  b += (uint32_t)k[5] << 8;   /* fall through */
    case 5:  b += k[4];                  /* fall through */
    case 4:  a += (uint32_t)k[3] << 24;  /* fall through */
    case 3:  a += (uint32_t)k[2] << 16;  /* fall through */
    case 2:  a += (uint32_t)k[1] << 8;   /* fall through */
    case 1:  a += k[0];
    }
    MIX(a, b, c);
    return c;
}
#undef MIX

/* A race's engage trigger: race_bin_NN/engage lists the bin's engage triggers (field 0x58DC14C0), each a collection
 * (engagetrigger_<kind>_<event>) whose field 0x277566F3 is the race's collection key and 0x9F743A0E its place.  The
 * trigger is near the race's start grid, but not always at it (race_bin_14's 14_1_1 circuit: about 150 m apart, and
 * Enter at the start grid opened nothing).  Read with the game's Attrib calls on gameplay instances made on the guest
 * stack (0x5E2580 makes one for a key, 0x454810 a field's value pointer, 0x5DCCA0 a position as simulation space,
 * 0x45A430 lets it go).  1 = found, its place in pos. */
#define FN_ATTRIB_INST   0x005e2580u
#define FN_ATTRIB_FIELD  0x00454810u
#define FN_ATTRIB_POS    0x005dcca0u
#define FN_ATTRIB_FREE   0x0045a430u
#define F_ENGAGE_LIST    0x58dc14c0u
#define F_TRIGGER_RACE   0x277566f3u
static int engage_trigger(CPU *c, int bin, uint32_t race_key, float pos[3]){
    char name[40]; snprintf(name, sizeof name, "race_bin_%02d/engage", bin);
    uint32_t k3[3] = { h32(name), 0, 0 };
    c->esp -= 0x80u;                    /* two instances and a vector below the game's stack, the calls below them */
    uint32_t inst = c->esp, inst2 = inst + 0x20u, out = inst + 0x40u;
    memset(g_mem + inst, 0, 0x80);
    call(c, FN_ATTRIB_INST, inst, 3, k3);
    int found = 0;
    for (uint32_t i = 0; i < 32 && !found; i++){
        uint32_t a2[2] = { F_ENGAGE_LIST, i }, p = call(c, FN_ATTRIB_FIELD, inst, 2, a2), tkey = rd(p);
        if (!p || !tkey) break;
        uint32_t t3[3] = { tkey, 0, 0 };
        memset(g_mem + inst2, 0, 0x20);
        call(c, FN_ATTRIB_INST, inst2, 3, t3);
        uint32_t b2[2] = { F_TRIGGER_RACE, 0 }, q = call(c, FN_ATTRIB_FIELD, inst2, 2, b2);
        if (q && rd(q) == race_key){
            uint32_t s2[2] = { inst2, out };
            call(c, FN_ATTRIB_POS, 0, 2, s2);
            memcpy(pos, g_mem + out, 12);
            found = 1;
        }
        call(c, FN_ATTRIB_FREE, inst2, 0, 0);
    }
    call(c, FN_ATTRIB_FREE, inst, 0, 0);
    c->esp += 0x80u;
    return found;
}

struct race { uint32_t hash, key, params, flags; int trig; float pos[3], dir[3]; };
static int read_races(CPU *c, uint32_t binobj, struct race *r, int max, int boss){
    uint32_t prof = profile(); int bin = prof ? MR8(prof + PROF_BIN) : 0;
    uint32_t db = rd(G_RACEDB);
    int n = binobj ? (int)call(c, boss ? FN_BIN_BOSSES : FN_BIN_RACES, binobj, 0, 0) : 0;
    if (n > max) n = max;
    for (int i = 0; i < n; i++){
        struct race *x = &r[i];
        memset(x, 0, sizeof *x);
        x->hash = call1(c, boss ? FN_BIN_BOSS : FN_BIN_RACE, binobj, (uint32_t)i);
        x->params = x->hash ? call1(c, FN_DB_PARAMS, db, x->hash) : 0u;
        x->flags = score_flags(x->hash);
        if (!x->params) continue;
        c->esp -= 0x20u;                /* the two vectors below the game's stack, the calls below them */
        uint32_t at = c->esp;
        memset(g_mem + at, 0, 0x20);
        call1(c, FN_START_POS, x->params, at);
        call1(c, FN_START_DIR, x->params, at + 12u);
        memcpy(x->pos, g_mem + at, 12); memcpy(x->dir, g_mem + at + 12u, 12);
        c->esp += 0x20u;
        /* the race's collection key (the list's element, as 0x601060 reads it), and its engage trigger's place */
        uint32_t a2[2] = { boss ? F_BIN_BOSS_LIST : F_BIN_RACE_LIST, (uint32_t)i }, p = call(c, FN_ATTRIB_FIELD, binobj, 2, a2);
        x->key = rd(p);
        float tp[3];
        if (x->key && engage_trigger(c, bin, x->key, tp)){ memcpy(x->pos, tp, sizeof tp); x->trig = 1; }
    }
    return n;
}
static void log_gates(const struct gates *g, const char *why){
    fprintf(stderr, "[mwbot] f=%ld t=%.1f gates (%s): bin %d races won %d/%d milestones %d/%d bounty %d/%d cash %d cars %d\n",
            autotest_frame(), mw_game_time(), why, g->bin, g->won, g->won_req, g->ms, g->ms_req, g->bounty, g->bounty_req,
            g->cash, g->cars);
}
/* the career's cars by name: a record's vehicle key (+8) is h32 of the car's pvehicle name, one of the 38 racers (GAME
 * 18); the current car is CareerSettings' first dword (profile +0xA8), a record's handle */
static const char *car_name(uint32_t key){
    static const char *const k[] = { "911turbo", "997s", "a3", "a4", "bmwm3gtr", "bmwm3gtre46", "camaro", "carreragt",
        "caymans", "clio", "clk500", "cobaltss", "corvette", "corvettec6r", "cts", "db9", "eclipsegt", "elise", "fordgt",
        "gallardo", "gti", "gto", "imprezawrx", "is300", "lancerevo8", "monaro", "murcielago", "mustanggt", "punto", "rx7",
        "rx8", "sl500", "sl65", "slr", "speedtest", "supra", "tt", "viper", 0 };
    for (int i = 0; k[i]; i++) if (h32(k[i]) == key) return k[i];
    return "?";
}
/* a car's tier, 1 the fastest: the Car Lot's three tiers as the game's design has them (a guide for the bot's choice of
 * car, not read from the game's data); a car not listed counts as tier 3 */
static int car_tier(uint32_t key){
    static const char *const t1[] = { "carreragt", "slr", "murcielago", "fordgt", "911turbo", "corvettec6r", "gallardo",
        "sl65", "db9", "bmwm3gtr", 0 };
    static const char *const t2[] = { "997s", "corvette", "viper", "clk500", "sl500", "caymans", "elise", "a4", "cts", "tt", 0 };
    const char *n = car_name(key);
    for (int i = 0; t1[i]; i++) if (!strcmp(n, t1[i])) return 1;
    for (int i = 0; t2[i]; i++) if (!strcmp(n, t2[i])) return 2;
    return 3;
}
/* how good a career car is for the bot: its tier first (worth more than all the package levels a car can hold), then
 * the package levels on it, then the later career record (the latest car won).  Up to bin 9 the bot raced the latest
 * car won; at bin 8 that is the Lancer Evolution, below the Cayman S won a bin earlier (bin8_start's cars: two IS300s,
 * Supra, RX-8, Cayman S, Lancer). */
static int car_rank(uint32_t prof, uint32_t r){
    int lv = car_levels(prof, MR32(r));
    return (4 - car_tier(rd(r + 8u))) * 1000 + (lv > 0 ? lv : 0) * 10 + MR8(r + 0x11u) % 10;
}
static void log_cars(void){
    uint32_t prof = profile(), cur = rd(prof + 0xa8u);
    char b[300]; int o = 0;
    for (uint32_t i = 0, r = prof + PROF_CARDB; prof && i < 200u && o < 260; i++, r += 0x14u)
        if (mem_is_committed(r + 0x13u) && MR32(r) != 0xffffffffu && MR8(r + 0x11u) != 0xffu)
            o += snprintf(b + o, sizeof b - (size_t)o, " %s (handle %#x, career record %u%s)", car_name(rd(r + 8u)), MR32(r),
                          MR8(r + 0x11u), MR32(r) == cur ? ", current" : "");
    fprintf(stderr, "[mwbot]   the career's cars:%s\n", o ? b : " none");
}
static void log_races(const struct race *r, int n){
    for (int i = 0; i < n; i++)
        fprintf(stderr, "[mwbot]   race %d hash %08x key %08x flags %x %s %.2f %.2f %.2f facing %.3f %.3f %.3f%s\n", i,
                r[i].hash, r[i].key, r[i].flags, r[i].trig ? "trigger at" : "start grid at", r[i].pos[0], r[i].pos[1],
                r[i].pos[2], r[i].dir[0], r[i].dir[1], r[i].dir[2], r[i].flags & 2u ? " (completed)" : "");
}

/* ---- the bot ---- */
enum { B_OFF, B_ROAM, B_GOTO, B_ENGAGE, B_RACE, B_PURSUIT, B_SAVE, B_SHOP, B_CAR, B_DONE };
static const char *const st_name[] = { "off", "roam", "goto", "engage", "race", "pursuit", "save", "shop", "car", "done" };
#define SETTLE_S      1.5               /* game seconds in free roam before the bot decides */
#define ENGAGE_TRIES  5                 /* Enters at a race's start before it is skipped for now */
#define ENGAGE_EVERY  90                /* frames between them */
#define RACE_START_MAX 6000             /* frames from Accept to the race going active */
#define PURSUIT_NATURAL_S 60.0          /* game seconds of free driving for a pursuit to start by itself */
#define CHASE_NATURAL_S   5.0           /* ... in bot chase */
#define BUST_FLOW_MAX     3600          /* frames from a bust's end of the pursuit to its infractions sheet, at most */
#define SKIP_MAX      3
#define BOSS_TRIES    12                /* the rival's challenge engaged at most this often (each lost one is a try) */
#define CAR_SWAP_AFTER 2                /* lost challenges in one car before the next try is in another (decide) */
#define CAR_SWAP_MAX  9000              /* frames for that change (B_CAR), at most */
#define RIVAL_NEXT_WAIT 1800            /* frames the bot waits in free roam for the rival's next race to come by itself */
#define POST_WAIT     2400              /* frames from a race's end to its result screen, at most (after it the bot goes on) */
#define AP_STAND      4                 /* career_mw.c's autopilot: the AI lets the car go now, and takes the next race at its GO */
#define GOTO_NEAR_M   20.0f             /* the car at a start: this near it (X, Z) */
static struct {
    int st, target, once;               /* once: a probe (bot race, bot pursuit) */
    double t0, free_since;              /* the state's start, free roam's start (game time) */
    long f0, last_tap;
    long fin_f;                         /* B_RACE: the frame the race ended for the player */
    int post_seen;                      /* and its result screen came */
    int race, tries, raced, go_set, focus0_set, skips[MAX_RACES], tried[MAX_RACES], boss_tries, boss_logged, warps;
    float tpos[3], tdir[3];             /* the start B_GOTO took the car to */
    int via, boss_engage_fail;          /* go_to: the car at another race's place first; a rival's engage opened nothing */
    uint32_t focus0, pursuit;
    int bounty0, forced;
    int stop_races;                     /* bot run N races [K]: stop once the races are won (a checkpoint to save) */
    int stop_wins;                      /* K: stop once K of the bin's races are won */
    int stop_ready;                     /* bot run N ready: stop once the rival's requirements are met */
    int after;                          /* B_PURSUIT: the state to go back to (0: B_ROAM, or the probe's end) */
    double next_log, escape_t;          /* escape_t: the game time of the pursuit's last escape */
    int escapes;                        /* and how many it had */
    float trail[16][3];                 /* pursuit_frame's trail (TRAIL_N) */
    float esc_pos[3], esc_dir[3];       /* the last escape's place, checked once the car is there (esc_check: tries) */
    int esc_check;
    int esc_recent[2], esc_dest;        /* pursuit_escape: the last two places (race index + 1) and the last one */
    unsigned fell_mask;                 /* race places whose warp left the car under the road */
    int hides, hidden;                  /* pursuit_frame: the car set in a hiding spot this pursuit, and standing there */
    int trail_n, rejoins, last_pst;
    double trail_t, search_t;
    float plen;                         /* the pursuit's length as it last showed it (s) */
    long run_f0;                        /* the frame the run began */
    /* B_SHOP (shop_frame, the shop's screens in mw_bot_present) */
    int shop_boss, boss_ended;          /* the rival's challenges ended in free roam (lost: a won one ends the bin), and
                                           that count at the last shop */
    int swap_at, car_swapped;           /* boss_ended at the last change of car (B_CAR), and that change done */
    int shop_bin, shop_cash, shop_tries, sh_started, sh_cat, sh_cat_done, sh_pk_new, sh_target, sh_entered, sh_paid, sh_inst;
    uint32_t want_car;                  /* the car to race (pick_car), 0 = the current one */
    int want_pos, car_entered;          /* its place in Car Select's list ('k / N'), and Enter given there */
    long sh_total0, sh_spent, sh_cash0;
    double chase_s, chase_acc, last_t;  /* bot chase: the game seconds asked, those in pursuits so far, the last frame's */
    int pursuits, busts, evades;
    long end_f;                         /* the frame the pursuit object went (0: on, or none yet) */
    int end_kind, sheet_seen;           /* how it ended (mw_aid_last_end), the infractions sheet seen since */
    /* the impound (M4-48): Out of Jail markers held (read in mw_bot_frame, used on the infractions sheet), those
     * spent, the sheet's moves toward the marker's button; Car_Select.fng after a bust: the place Enter was given at */
    int oojail, jail_spent, jail_moves, cs_k, cs_dlg, cs_retry, impounds;
    long mk_f;
    /* Car Select's list: the car each place holds, as seen (cs_pos_for), for cs_map_n cars; the place Enter was given
     * at, and the tries at the car picked */
    uint32_t cs_map[33];
    int cs_map_n, cs_ek, cs_tries;
    int stand, bust_n, pay_only;        /* bot bust N [pay]: the car stands in the pursuits until N busts */
    int as_on, as_bin, as_won, as_ms, as_ready, as_resume, as_n;   /* NFSMW_BOT_AUTOSAVE: saves on the way (decide) */
    int quick, quick_tries, quick_ok, quick_bin, bin_pursuits;   /* a quick pursuit for the escape-within milestone
                                           (decide, pursuit_frame): on now, tried, met; the bin they count in */
    int unsticks;                       /* unstick: the standing car set on a road before a forced pursuit, this time */
    int rejoin_stand;                   /* pursuit_frame: the car stands where a rejoin set it (since rejoin_t) */
    double rejoin_t;
    struct gates g;
} B;
static void set_state(int st, const char *why){
    fprintf(stderr, "[mwbot] f=%ld t=%.1f %s -> %s%s%s\n", autotest_frame(), mw_game_time(), st_name[B.st], st_name[st],
            why ? ": " : "", why ? why : "");
    B.st = st; B.t0 = mw_game_time(); B.f0 = autotest_frame(); B.tries = 0; B.free_since = 0; B.go_set = 0;
    B.fin_f = 0; B.post_seen = 0;
}
/* The car the bot races: the career's best by car_rank (a beaten rival's pink slip comes with packages of its own:
 * Taz's IS300 with engine, transmission, nitrous and brakes at level 1).  When it is not the current car, the
 * safehouse menu's Car Select (its third item) takes it before free roam: Car_Select.fng lists the cars ('k / N',
 * strings of label 0x18; it opens on the current car), mostly in the order of their career records but not always
 * (cs_pos_for), RIGHT to the next, and Enter makes the highlighted car the current one and goes back to the safehouse
 * menu (probe of 2026-10-30: Enter on '2 / 3' made handle 0x52 current). */
/* The car taken is one that can be driven (its career record's +4 is 0): Enter on an impounded one opens a Dialog
 * and leaves Car_Select.fng as it was (M4-48: the bot went round that loop for good, runs c13d and c13t). */
/* the car of the best car_rank that can be driven (PREFER instead, when it can be driven), with its career record
 * (returned), its handle and its place in Car Select's list ('k / N': the list is in career-record order) */
static int choose_car(uint32_t prefer, uint32_t *handle, int *pos, int *n_out){
    uint32_t prof = profile(), best_h = 0;
    int best = -1, best_rank = -1, n = 0, recs[32], pref = 0;
    for (uint32_t i = 0, r = prof + PROF_CARDB; prof && i < 200u && n < 32; i++, r += 0x14u)
        if (mem_is_committed(r + 0x13u) && MR32(r) != 0xffffffffu && MR8(r + 0x11u) != 0xffu){
            int rec = MR8(r + 0x11u), rank = car_rank(prof, r);
            recs[n++] = rec;
            if (pref || !car_free(prof, MR32(r))) continue;
            if (prefer && MR32(r) == prefer){ best = rec; best_h = prefer; pref = 1; }   /* PREFER, when it can be driven */
            else if (rank > best_rank){ best = rec; best_rank = rank; best_h = MR32(r); }
        }
    int p = 1;
    for (int i = 0; i < n; i++) p += recs[i] < best;
    *handle = best_h; *pos = p; *n_out = n;
    return best;
}
/* the car for the next rival challenge after CAR_SWAP_AFTER lost in one car: from the best car (choose_car's) the
 * second best, from any other car the best again, so the two best take turns (the shop buys only for the best,
 * shop_wanted); 0 when there is no other that can be driven */
static uint32_t next_car(void){
    uint32_t prof = profile(), cur = rd(prof + 0xa8u), best_h = 0, h2 = 0;
    int pos = 0, n = 0, r2 = -1;
    choose_car(0, &best_h, &pos, &n);
    if (best_h && best_h != cur) return best_h;
    for (uint32_t i = 0, r = prof + PROF_CARDB; prof && i < 200u; i++, r += 0x14u)
        if (mem_is_committed(r + 0x13u) && MR32(r) != 0xffffffffu && MR8(r + 0x11u) != 0xffu && MR32(r) != cur &&
            car_free(prof, MR32(r)) && car_rank(prof, r) > r2){ r2 = car_rank(prof, r); h2 = MR32(r); }
    return h2;
}
static void pick_car(void){
    uint32_t prof = profile(), cur = rd(prof + 0xa8u), best_h = 0;
    int pos = 0, n = 0, best = choose_car(0, &best_h, &pos, &n);
    B.want_car = 0; B.car_entered = 0;
    for (uint32_t i = 0, r = prof + PROF_CARDB; prof && i < 200u; i++, r += 0x14u)   /* the ranks, for the log */
        if (mem_is_committed(r + 0x13u) && MR32(r) != 0xffffffffu && MR8(r + 0x11u) != 0xffu)
            fprintf(stderr, "[mwbot]   car %s (handle %#x, career record %u): tier %d, package levels %d, rank %d%s%s\n",
                    car_name(rd(r + 8u)), MR32(r), MR8(r + 0x11u), car_tier(rd(r + 8u)), car_levels(prof, MR32(r)),
                    car_rank(prof, r), car_free(prof, MR32(r)) ? "" : ", impounded", MR32(r) == cur ? ", current" : "");
    if (!best_h || best_h == cur) return;
    B.want_car = best_h; B.want_pos = pos;
    fprintf(stderr, "[mwbot] f=%ld the car to race: handle %#x (career record %d), Car Select's %d / %d\n", autotest_frame(),
            best_h, best, pos, n);
}
/* the car the bot races best (choose_car's), for the shop: 0 none */
static uint32_t best_car(void){ uint32_t h = 0; int pos = 0, n = 0; choose_car(0, &h, &pos, &n); return h; }
/* Car Select's place for car WANT among N: the place seen holding it, else GUESS (choose_car's, the career-record
 * order) when no other car was seen there, else the free place nearest to it.  The list is not always in career-record
 * order: at bin 8 (six cars, the current one the RX-8 at '4 / 6') RIGHT to '5 / 6' and Enter took the Lancer, career
 * record 5, not the Cayman S, record 4 (c5-s8r).  What a place holds is seen twice: the list opens on the current car,
 * and the car an Enter took is the current one after it. */
static void cs_map_check(int n){ if (n != B.cs_map_n){ memset(B.cs_map, 0, sizeof B.cs_map); B.cs_map_n = n; } }
static int cs_pos_for(uint32_t want, int guess, int n){
    cs_map_check(n);
    for (int p = 1; p <= n && p < 33; p++) if (B.cs_map[p] == want) return p;
    if (guess >= 1 && guess <= n && guess < 33 && !B.cs_map[guess]) return guess;
    for (int d = 1; d < n; d++){
        int p = guess + d;
        if (p >= 1 && p <= n && p < 33 && !B.cs_map[p]) return p;
        p = guess - d;
        if (p >= 1 && p <= n && p < 33 && !B.cs_map[p]) return p;
    }
    return guess;
}
/* a race runs for the local player: GRaceStatus actively racing and its racer not finished.  After a won career race
 * the flag stays set in the free roam that follows (seen: active 1, the racer finished and the race won, through
 * 25,000 frames of free roam, until the next load), so the racer's finished byte tells the race's end. */
static int racing(CPU *c){
    uint32_t rs = rd(G_RACESTATUS), s = mw_aid_simable(c), a = s;
    if (!rs || !MR8(rs + RS_ACTIVE)) return 0;
    int idx = s ? (int)call(c, FN_RACER_INDEX, 0, 1, &a) : -1;
    if (idx < 1 || idx > 16) return 1;  /* a race without the player in it yet: the countdown's set-up */
    return !MR8(rs + RS_RACERS + (uint32_t)(idx - 1) * RACER_SIZE + RI_FINISHED);
}
/* free roam: the world, no race running, no screen taking input, a car */
static int free_roam(CPU *c){
    if (game_flow() != g_game.flow_race) return 0;
    if (mw_fe_input_top(0)) return 0;
    if (!mw_aid_simable(c)) return 0;
    return !racing(c) && !mw_aid_pursuit(c, 0);
}
/* the cops on the car in free roam (a pursuit that began in a race goes on after it: there Enter opens the pursuit's
 * sheet, PostRace_Pursuit.fng, no event engages, and the pause menu has no Jump to Safe House): the bot's pursuit
 * state takes it, the AI driving, and comes back to `after` once it is over */
static int pursuit_on(CPU *c, int after){
    if (game_flow() != g_game.flow_race || mw_fe_input_top(0) || racing(c) || !mw_aid_pursuit(c, 0)) return 0;
    B.after = after;
    set_state(B_PURSUIT, "the cops are on the car");
    return 1;
}
/* free roam for SETTLE_S game seconds now (the world after a load, a race's end) */
static int settled(CPU *c){
    if (!free_roam(c)){ B.free_since = 0; return 0; }
    double t = mw_game_time();
    if (B.free_since <= 0.0 || t < B.free_since){ B.free_since = t > 0.0 ? t : 1e-9; return 0; }
    return t - B.free_since >= SETTLE_S;
}

/* to a race's start: the car set there (the warp of Game.WarpPlayerToTrigger), standing; B_GOTO checks it got there */
/* A car already at the start (a lap knockout or circuit lost ends at its start line, inside the engage trigger) got
 * no engage dialog from three warps of five Enters each after the loss, and the run ended there (c4-bl15r3-2: 15_3_1
 * lost, then 'no engage dialog' three times; the first engage of the same run, from far away, opened it at once).
 * Inferred: the trigger engages a car that comes into it, and a warp from inside it to inside it is not one.  So when
 * the car is within VIA_NEAR_M of the start, or a try at this race opened nothing, it is set first at the place of
 * another of the bin's races (a road place) and settles there, then at the start. */
#define VIA_NEAR_M    150.0f
static void go_to(CPU *c, const struct race *r, const struct race *all, int n, int failed){
    float p[3], kmh = 0;
    mw_aid_autopilot(c, AP_STAND);             /* the car stands still at the start */
    memcpy(B.tpos, r->pos, sizeof B.tpos); memcpy(B.tdir, r->dir, sizeof B.tdir);
    B.via = 0;
    float d0 = mw_aid_where(c, p, &kmh) ? hypotf(p[0] - r->pos[0], p[2] - r->pos[2]) : 1e9f;
    if (d0 < VIA_NEAR_M || failed){
        int best = -1; float bd = 0;
        for (int i = 0; i < n; i++){
            float d = hypotf(all[i].pos[0] - r->pos[0], all[i].pos[2] - r->pos[2]);
            if (all[i].params && d > VIA_NEAR_M && d > bd){ bd = d; best = i; }
        }
        if (best >= 0){
            B.via = 1;
            fprintf(stderr, "[mwbot] f=%ld t=%.1f the car is %.0f m from the start%s: first to race %d's place, %.0f m from it\n",
                    autotest_frame(), mw_game_time(), d0, failed ? " (a try there opened nothing)" : "", best, bd);
            mw_aid_teleport(c, all[best].pos, all[best].dir);
        }
    }
    if (!B.via) mw_aid_teleport(c, r->pos, r->dir);
    B.warps = 1;
    set_state(B_GOTO, 0);
}

/* ---- the customization shop: performance packages bought with the career's cash ----
 * The shop is customshop_2 (GLOBAL/gameplay.bin menu_gate_customshop/zone_customshop_2, sim (-2563.89, 157.62,
 * 1509.35)), engaged as career_shop.txt does it: the car set inside its zone, Enter, CUSTOMIZATION_SHOP.BIN and
 * CustomizeMain.fng.  Performance (0xBAEF8282, its second item 0x4B4E388B) opens CustomizeCategory.fng's seven
 * packages, items 0x4B4E388A on: engine, transmission, suspension, nitrous, tires, brakes, turbo.  Each opens
 * CustomizePerformance.fng, the package's levels (3 or 4, 'k / N' in the strings of label 0x18, RIGHT to the next)
 * with the highlighted level's cost (0x7D5ABBC0 'cost'), the funds (0x073B79E0), the cart's total (0x4C8EB254) and
 * the trade-in of the part on the car (0x8CDCB8ED).  Seen at bin 13 (cash 47,950, stock parts): engine 5,500,
 * 14,200, 29,800, 98,000; transmission 4,800 to 75,200; suspension 3,600, 7,600, 10,000; nitrous 4,900 to 18,000;
 * tires 1,400 to 11,800; brakes 3,100 to 26,800; turbo 5,400, 25,000, 76,000.  Enter on a level that is locked
 * (later rivals unlock them) or beyond the funds does nothing: the total stays.  So the bot goes to the dearest level
 * the money left (less SHOP_RESERVE) reaches, Enter, and reads the total: risen, the level is in the cart and the
 * package is done (Escape); else the next lower level, down to the first.  A level is tried only when it costs more
 * than twice the trade-in of the part on the car (no step down).  After the seventh package Escape opens
 * ShoppingCart.fng when the cart holds something: Enter checks out, Dialog.fng's Yes pays, CustomizeMain.fng comes
 * back; Escape there leaves the shop for free roam.  The bot shops when a bin begins (its first decision) and again
 * when the cash has grown by SHOP_AGAIN since. */
#define SHOP_RESERVE  8000              /* cash kept back (a bust's fines, an impounded car's release) */
#define SHOP_MIN      1400              /* the cheapest level seen */
#define SHOP_AGAIN    15000
#define SHOP_CATS     7
#define SHOP_TRIES    5
static const char *const shop_cat_name[SHOP_CATS] = { "engine", "transmission", "suspension", "nitrous", "tires", "brakes", "turbo" };
/* the order the bot goes through them (the screen's item = 0x4B4E388A + the package): what makes the car faster first
 * (engine, turbo, transmission, nitrous, tires), the cash left for the rest; the screen's order spent the cash of bin
 * 10's shop on engine, transmission, suspension and nitrous (c4-b10r) before turbo came */
static const int shop_order[SHOP_CATS] = { 0, 6, 1, 3, 4, 2, 5 };
static int shop_cat(void){ return B.sh_cat >= 0 && B.sh_cat < SHOP_CATS ? shop_order[B.sh_cat] : -1; }
static const float shop_pos[3] = { -2563.89f, 157.62f, 1509.35f }, shop_dir[3] = { 1.0f, 0.0f, 0.0f };
/* the level of a package on the current car: its customization record (FEPlayerCarDB +0xFA0, 0x198 bytes each, the
 * car record's byte +0x10 its index) holds the installed levels as seven dwords at +0x118 (all 1 after the seven
 * first levels were bought), in the game's order tires, brakes, chassis, transmission, engine, induction, nitrous
 * (the shop's packages map to them by shop_type; inferred from the order of MW's upgrade types, the transmission
 * and brakes at level 2 after a bin-12 shop checked it); -1 when there is no such record.  The shop's own screens
 * do not tell it: the trade-in shows 0 with parts on the car, and an installed level takes no Enter, like a locked
 * one, so without it the bot would step past the installed level to a lower one, which the cart takes. */
static const int shop_type[SHOP_CATS] = { 4, 3, 2, 6, 0, 1, 5 };
static int installed_level(int cat){
    uint32_t prof = profile(), cur = rd(prof + 0xa8u);
    if (!prof || cat < 0 || cat >= SHOP_CATS) return -1;
    for (uint32_t i = 0, r = prof + PROF_CARDB; i < 200u; i++, r += 0x14u)
        if (mem_is_committed(r + 0x13u) && MR32(r) == cur && MR8(r + 0x10u) < 0x4bu)
            return (int)rd(prof + PROF_CARDB + 0xfa0u + MR8(r + 0x10u) * 0x198u + 0x118u + 4u * (uint32_t)shop_type[cat]);
    return -1;
}
/* and again after a lost rival's challenge (the two races are one challenge; bin 11's was lost three times in a row
 * by a few seconds, c4-b11a), when the money left reaches a level */
static int swap_due(void){ return !B.once && B.boss_ended >= CAR_SWAP_AFTER && B.boss_ended - B.swap_at >= CAR_SWAP_AFTER; }
static int shop_wanted(const struct gates *g){
    if (B.once || g->cash < SHOP_RESERVE + SHOP_MIN) return 0;
    /* only for the best car (choose_car's): the packages bought for a car the bot left after two lost challenges were
     * lost to it (bins 10 and 9: 46,500 and 29,800 spent on cars raced in one challenge, c4-b10r, c4-b9r) */
    uint32_t cur = rd(profile() + 0xa8u), best = best_car();
    if (best && cur != best) return 0;
    /* (not for a car about to be changed: decide's swap first, then the shop for the car taken) */
    return B.shop_bin != g->bin || g->cash >= B.shop_cash + SHOP_AGAIN || (B.boss_ended > B.shop_boss && !swap_due());
}
static void go_shop(CPU *c, const struct gates *g){
    B.shop_boss = B.boss_ended;
    B.sh_started = B.sh_cat = B.sh_cat_done = B.sh_pk_new = B.sh_entered = B.sh_paid = 0; B.sh_target = 0;
    B.sh_spent = 0; B.sh_cash0 = g->cash;
    fprintf(stderr, "[mwbot] f=%ld t=%.1f the shop: cash %d, to customshop_2\n", autotest_frame(), mw_game_time(), g->cash);
    mw_aid_autopilot(c, AP_STAND);
    memcpy(B.tpos, shop_pos, sizeof B.tpos); memcpy(B.tdir, shop_dir, sizeof B.tdir);
    mw_aid_teleport(c, shop_pos, shop_dir);
    set_state(B_SHOP, "performance packages");
}
static void shop_frame(CPU *c){
    long f = autotest_frame();
    if (!B.sh_started){                 /* at the gate: Enter until the shop's menu comes (mw_bot_present takes it) */
        if (pursuit_on(c, B_ROAM)) return;
        if (mw_fe_input_top(0) || !settled(c) || f - B.last_tap < ENGAGE_EVERY) return;
        if (B.tries >= SHOP_TRIES){
            B.shop_bin = B.g.bin; B.shop_cash = B.g.cash;   /* not again in this bin (or until SHOP_AGAIN more) */
            set_state(B_ROAM, "the shop did not open"); return;
        }
        autotest_tap(DIK_RETURN, 6); B.last_tap = f; B.tries++;
        return;
    }
    if (!settled(c)) return;            /* left the shop: free roam again */
    struct gates g; read_gates(c, &g);
    fprintf(stderr, "[mwbot] f=%ld t=%.1f shop over: spent %ld, cash %ld -> %d\n", f, mw_game_time(), B.sh_spent, B.sh_cash0,
            g.cash);
    B.shop_cash = g.cash;
    set_state(B_ROAM, "shop over");
}
/* a number in what a screen shows (mw_fe_shows): the value of the first string after the one that begins with KEY
 * ("'cost" ... ":9588bd38'5,500"), commas skipped; -1 when not there */
static long shows_num(const char *s, const char *key){
    const char *k = strstr(s, key);
    if (!k) return -1;
    const char *q = strchr(k + strlen(key), ':');
    q = q ? strchr(q, '\'') : 0;
    if (!q) q = strchr(k + strlen(key), '^');
    if (!q) return -1;
    long v = 0; int any = 0;
    for (q++; *q && *q != ' '; q++){ if (*q >= '0' && *q <= '9'){ v = v * 10 + (*q - '0'); any = 1; } else if (*q != ',') break; }
    return any ? v : -1;
}
/* the i-th string of label LABEL ("00000018") as a number, -1 when not there */
static long shows_nth(const char *s, const char *label, int i){
    char k[16]; snprintf(k, sizeof k, ":%s", label);
    for (const char *p = s; (p = strstr(p, k)); p += strlen(k))
        if (i-- == 0){ const char *q = p + strlen(k) + 1; return *q >= '0' && *q <= '9' ? strtol(q, 0, 10) : -1; }
    return -1;
}
int mw_fe_shows(char *out, int cap);                         /* fe_mw.c */

static void milestones_log(CPU *c, int bin);
static float evasion_goal(CPU *c, int bin);
static int ms_evasion(int bin, int *others);
static void save_begin(void);
/* NFSMW_BOT_AUTOSAVE=1 (bot run): a save at the safehouse whenever the bin has moved on since the last one (two more
 * races won, a milestone more, the rival's requirements met, a new bin), then on from the safehouse menu.  A run the
 * game ends (a trap: c6-b5r, heat 4, after 3,500 s) then leaves a save tree near where it stopped (--save-out is
 * written after a trap too), not the seed it began with. */
static int autosave_due(const struct gates *g){
    if (!B.as_on || !B.target) return 0;
    int ready = g->won >= g->won_req && g->ms >= g->ms_req && g->bounty >= g->bounty_req;
    if (B.as_bin != g->bin){                /* the run's first decide, or a new bin (saved at once: the bin is the gate) */
        int first = B.as_bin == 0;
        B.as_bin = g->bin; B.as_won = g->won; B.as_ms = g->ms; B.as_ready = ready;
        return !first;
    }
    if (g->won >= B.as_won + 2 || g->ms > B.as_ms || (ready && !B.as_ready)){
        B.as_won = g->won; B.as_ms = g->ms; B.as_ready = ready;
        return 1;
    }
    return 0;
}
#define QUICK_LEN     5.0f              /* a quick pursuit: the chase broken off at this length (s) */
#define QUICK_MAX     10                /* quick pursuits in one bin, at most */
static void decide(CPU *c){
    struct gates g; struct race r[MAX_RACES];
    if (!read_gates(c, &g)){ set_state(B_DONE, "no career"); return; }
    if (memcmp(&g, &B.g, sizeof g)){ log_gates(&g, "now"); if (g.cars != B.g.cars) log_cars(); }
    if (g.bin != B.g.bin) milestones_log(c, g.bin);
    B.g = g;
    if (autosave_due(&g) && !(B.target && g.bin <= B.target) && !(B.stop_ready && g.won >= g.won_req && g.ms >= g.ms_req &&
                                                                   g.bounty >= g.bounty_req)){
        B.as_n++; B.as_resume = 1;
        fprintf(stderr, "[mwbot] f=%ld t=%.1f autosave %d: bin %d races won %d milestones %d\n", autotest_frame(), mw_game_time(),
                B.as_n, g.bin, g.won, g.ms);
        save_begin(); set_state(B_SAVE, "autosave (NFSMW_BOT_AUTOSAVE)"); return;
    }
    if (B.target && g.bin <= B.target){
        fprintf(stderr, "[mwbot] f=%ld t=%.1f reached bin %d (%ld frames after bot run)\n", autotest_frame(), mw_game_time(),
                g.bin, autotest_frame() - B.run_f0);
        set_state(B_DONE, "the target bin"); return;
    }
    if (B.stop_wins && g.won >= B.stop_wins){ set_state(B_DONE, "the races asked for are won (bot run N races K)"); return; }
    if (B.stop_ready && g.won >= g.won_req && g.ms >= g.ms_req && g.bounty >= g.bounty_req){
        set_state(B_DONE, "the rival's requirements are met (bot run N ready)"); return;
    }
    if (shop_wanted(&g)){ go_shop(c, &g); return; }
    int n = read_races(c, g.binobj, r, MAX_RACES, 0), pick = -1;
    if (g.won < g.won_req)              /* the race not won yet that was tried the least (a lost race waits its turn) */
        for (int i = 0; i < n; i++)
            if (!(r[i].flags & 2u) && r[i].params && B.skips[i] < SKIP_MAX && (pick < 0 || B.tried[i] < B.tried[pick])) pick = i;
    if (pick >= 0){
        B.race = pick; B.tried[pick]++;
        fprintf(stderr, "[mwbot] f=%ld t=%.1f race %d (hash %08x, try %d): to its start %.2f %.2f %.2f\n", autotest_frame(),
                mw_game_time(), pick, r[pick].hash, B.tried[pick], r[pick].pos[0], r[pick].pos[1], r[pick].pos[2]);
        go_to(c, &r[pick], r, n, B.skips[pick] > 0); return;
    }
    if (g.won < g.won_req){ log_races(r, n); set_state(B_DONE, "races to win, none left to engage"); return; }
    if (B.stop_races){ set_state(B_DONE, "the races the rival asks are won (bot run N races)"); return; }
    if (g.ms < g.ms_req || g.bounty < g.bounty_req){
        /* a bin with a pursuit_evasion_time milestone: its first pursuit and every other one after it is a quick one
         * (pursuit_frame: the chase broken off at QUICK_LEN, the car hidden at once), until one of them raised the
         * milestones, QUICK_MAX of them */
        if (B.quick_bin != g.bin){ B.quick_bin = g.bin; B.quick_tries = 0; B.quick_ok = 0; B.bin_pursuits = 0; }
        int others = 0, ev = ms_evasion(g.bin, &others);
        B.quick = g.ms < g.ms_req && ev && ev != 4 && B.quick_tries < QUICK_MAX &&
                  (B.bin_pursuits % 2 == 0 || (!others && g.bounty >= g.bounty_req)) && evasion_goal(c, g.bin) > 0.0f;
        B.quick_tries += B.quick;
        B.bin_pursuits++;
        set_state(B_PURSUIT, B.quick ? "milestones to earn: a quick pursuit (escape within)" : "milestones or bounty to earn");
        return;
    }
    /* the rival's challenge: its first race is engaged at its start like the others */
    struct race b[8];
    int nb = read_races(c, g.binobj, b, 8, 1);
    if (!B.boss_logged){ B.boss_logged = 1; log_races(r, n); fprintf(stderr, "[mwbot]   the rival's races:\n"); log_races(b, nb); }
    int done = 0;
    for (int i = 0; i < nb; i++) done += (b[i].flags & 2u) != 0;
    if (done && done < nb && autotest_frame() - B.f0 < RIVAL_NEXT_WAIT) return;   /* its next race may come by itself */
    /* Two challenges lost in one car: the next try in another car (the career's next car that can be driven, by
     * career record, highest first, round), taken at the safehouse's Car Select (B_CAR).  Bin 11's rival beat the
     * bot's RX8 four times in a row (c4-b11a), each time in the second race, twice wedged at the same place. */
    if (swap_due()){
        uint32_t h = next_car();
        B.swap_at = B.boss_ended;
        if (h){
            int pos = 0, nn = 0; uint32_t hh = 0;
            choose_car(h, &hh, &pos, &nn);
            B.want_car = h; B.want_pos = pos; B.car_entered = 0; B.car_swapped = 0;
            B.shop_boss = B.boss_ended - 1;     /* then the shop, for the car taken */
            fprintf(stderr, "[mwbot] f=%ld t=%.1f %d of the rival's challenges lost: the next one in car %#x (Car Select's %d / %d), "
                    "taken at the safehouse\n", autotest_frame(), mw_game_time(), B.boss_ended, h, pos, nn);
            set_state(B_CAR, "another car for the rival");
            return;
        }
    }
    if (nb > 0 && b[0].params && B.boss_tries < BOSS_TRIES){   /* a lost challenge is raced again, as a lost race */
        B.boss_tries++; B.race = -1;
        fprintf(stderr, "[mwbot] f=%ld t=%.1f the rival's challenge (hash %08x, try %d, races won %d of %d): to its "
                "start %.2f %.2f %.2f\n", autotest_frame(), mw_game_time(), b[0].hash, B.boss_tries, done, nb, b[0].pos[0],
                b[0].pos[1], b[0].pos[2]);
        go_to(c, &b[0], r, n, B.boss_engage_fail); return;
    }
    set_state(B_DONE, "the rival's requirements are met; the challenge did not engage");
}

/* a bin's top heat (gameplay.bin's MaxHeatLevel, GAME 13.7: 15-13 2, 12-9 3, 8-5 4, 4-1 5; the prologue's 1) */
static int bin_max_heat(int bin){ return bin >= 16 ? 1 : bin >= 13 ? 2 : bin >= 9 ? 3 : bin >= 5 ? 4 : 5; }
/* a chase broken off (pursuit_frame): the car to the place of the bin's race farthest from it */
#define ESCAPE_LEN    300.0f            /* the chase's length (s) at which it is broken off */
#define ESCAPE_LEN_HEAT4 600.0f         /* ... from bin 8 on (escape_len) */
#define ESCAPE_BUST   1.0f              /* or the bust timer (s; 5 = busted) */
#define ESCAPE_GAP_S  20.0              /* game seconds between two escapes of one pursuit (10 when boxed in) */
#define ESCAPE_MAX    24                /* escapes in one pursuit (6 until bin 8, where a 550 s chase used them all, its
                                           bust timer at 1.0 again and again, and ended busted, c5-s8t; with 10 a
                                           1074 s chase spent them before its 630 s and ended busted too, c5-s8u) */
#define ESCAPE_FAR_M  800.0f
#define ESC_HIDE_MAX  6                 /* hiding spots in one pursuit once the chase has run escape_len */
#define ICON_HIDING   11                /* GManager's icon type HIDING_SPOT_%d */
int mw_aid_icon_place(int type, int k, float pos[3]);   /* pursuit_mw.c: icon k of type T, the world's axes */
#define TRAIL_N       16                /* the chase's trail: the car's place each second while the cops are on it */
#define REJOIN_S      4.0               /* game seconds of the cops' search before the car goes back */
#define REJOIN_BACK   3                 /* to where it was this many seconds before the search */
#define REJOIN_MAX    8                 /* times in one pursuit */
#define REJOIN_STAND_S 10.0             /* game seconds the car stands where a rejoin set it, at most */
/* The chase's length at which it is broken off, by bin: ESCAPE_LEN, or longer when the bin has a pursuit_length
 * milestone (gameplay.bin milestones/bin_NN/challenge_N_pursuit_length): its goal is drawn between two values, fields
 * 0x8445AF47 and 0x3B9BBFC2 (bin 8: 360 and 600 s, bin 6: 420 and 660; bins 12 and 10 480 and 540 at most, bins 3 to 1
 * 720 to 840), and a chase broken off at 300 s met it in none of bin 8's eight pursuits (c5-s8t: milestones 2 of 4,
 * evaded at 293, 358 and 192 s).  So there it is the larger value and 30 s.  Read with the game's Attrib calls as
 * engage_trigger reads a trigger; 0x3B9BBFC2 is a float. */
#define F_MS_GOAL_HI     0x3b9bbfc2u
static float escape_len(CPU *c, int bin){
    static int cached_bin = -1; static float cached = ESCAPE_LEN;
    if (bin == cached_bin) return cached;
    char name[64]; snprintf(name, sizeof name, "milestones/bin_%02d/challenge_%d_pursuit_length", bin, bin);
    uint32_t k3[3] = { h32(name), 0, 0 };
    c->esp -= 0x40u;
    uint32_t inst = c->esp;
    memset(g_mem + inst, 0, 0x40);
    call(c, FN_ATTRIB_INST, inst, 3, k3);
    uint32_t a2[2] = { F_MS_GOAL_HI, 0 }, p = call(c, FN_ATTRIB_FIELD, inst, 2, a2);
    float hi = p ? rdf(p) : 0.0f;
    call(c, FN_ATTRIB_FREE, inst, 0, 0);
    c->esp += 0x40u;
    cached_bin = bin;
    /* from heat 4 on (bins 8 to 1) at least ESCAPE_LEN_HEAT4: bin 5's milestones are counted in one pursuit (cost to
     * state 100,000 to 250,000, spike strips 6 to 12, the cops destroyed), and 300 s chases, each ended in a hiding
     * spot, met two of four in seven pursuits (c5-s5a, c5-s5b); bin 8's 869 s chase met all four */
    float base = bin <= 8 ? ESCAPE_LEN_HEAT4 : ESCAPE_LEN;
    cached = hi > 0.0f && hi < 3600.0f && hi + 30.0f > base ? hi + 30.0f : base;
    fprintf(stderr, "[mwbot] f=%ld bin %d: %s, chases broken off at %.0f s\n", autotest_frame(), bin,
            hi > 0.0f ? "a pursuit_length milestone" : "no pursuit_length milestone", (double)cached);
    if (hi > 0.0f) fprintf(stderr, "[mwbot]   (its goal at most %.0f s)\n", (double)hi);
    return cached;
}
/* The bin's pursuit_evasion_time milestone ("Escape within", the chase's length at the evade: PostRace_Pursuit.fng's
 * Escape_Within showed 9:04.78 for a chase of 545 s, c5-s5c): its first goal (0x8445AF47; the goals go down by 30 s to
 * 0x3B9BBFC2: bins 5, 3 and 1 120 to 60 s, bin 7 180 to 90).  0 when the bin has none.  The chases of escape_len
 * (600 s from heat 4 on) never meet it: in c5-s5a and c5-s5b bin 5 stood at 3 and 1 of its 4 milestones after 8 and
 * 7 pursuits, the others (bounty, cost to state, spike strips) met by the long ones. */
static float evasion_goal(CPU *c, int bin){
    static int cached_bin = -1; static float cached;
    if (bin == cached_bin) return cached;
    char name[80]; snprintf(name, sizeof name, "milestones/bin_%02d/challenge_%d_pursuit_evasion_time", bin, bin);
    uint32_t k3[3] = { h32(name), 0, 0 };
    c->esp -= 0x40u;
    uint32_t inst = c->esp;
    memset(g_mem + inst, 0, 0x40);
    call(c, FN_ATTRIB_INST, inst, 3, k3);
    uint32_t a2[2] = { 0x8445af47u, 0 }, p = call(c, FN_ATTRIB_FIELD, inst, 2, a2);
    uint32_t a3[2] = { F_MS_GOAL_HI, 0 }, q = call(c, FN_ATTRIB_FIELD, inst, 2, a3);
    float lo = p ? rdf(p) : 0.0f, hi = q ? rdf(q) : 0.0f;
    call(c, FN_ATTRIB_FREE, inst, 0, 0);
    c->esp += 0x40u;
    cached_bin = bin;
    cached = lo > 0.0f && lo < 3600.0f ? lo : 0.0f;
    fprintf(stderr, "[mwbot] f=%ld bin %d: %s", autotest_frame(), bin, cached > 0.0f ? "a pursuit_evasion_time milestone" :
            "no pursuit_evasion_time milestone");
    if (cached > 0.0f) fprintf(stderr, " (escape within %.0f to %.0f s): every other pursuit for milestones is a quick one\n",
                               (double)lo, (double)hi);
    else fprintf(stderr, "\n");
    return cached;
}
/* The bin's milestones: gameplay.bin milestones/bin_NN/challenge_N_<kind> has the goals (0x8445AF47 the first, the
 * one the game sets, 0x3B9BBFC2 the last, a step between), and the game keeps a record of 0x14 bytes for each (found
 * by its collection key in a block GManager [0x91E00C] points at; V c6-b5r): +0 the key, +4 flags (byte 0 the state:
 * 1 a later bin's, 2 open, 4 met; byte 1: 1 for "at least", 0 for "at most" (pursuit_evasion_time); byte 2 the bin),
 * +8 the goal (float), +0xC the best value so far (float), +0x10 the kind's key.  Found once a bin (ms_tab). */
#define F_MS_GOAL_LO     0x8445af47u
static const char *const ms_kinds[] = { "cops_damaged", "pursuit_evasion_time", "pursuit_length", "total_infractions",
    "cost_to_state", "cops_destroyed", "roadblocks_dodged", "tire_spikes_dodged", "cops_hit", "infractions" };
#define MS_KINDS (int)(sizeof ms_kinds / sizeof ms_kinds[0])
#define MS_EVASION 1
static struct { int bin, n; struct { int kind; uint32_t key, rec; } m[MS_KINDS]; } ms_tab = { -1, 0, {{0, 0, 0}} };
static uint32_t ms_find(uint32_t key, int bin){
    uint32_t gm = rd(0x0091e00cu);
    for (uint32_t go = 0; gm && go < 0x400u; go += 4u){
        uint32_t blk = rd(gm + go);
        if (blk < 0x00400000u || !mem_is_committed(blk)) continue;
        for (uint32_t bo = 0; bo < 0x8000u && mem_is_committed(blk + bo + 0x13u); bo += 4u)
            if (MR32(blk + bo) == key && (int)((MR32(blk + bo + 4u) >> 16) & 0xffu) == bin && MR32(blk + bo + 0x10u))
                return blk + bo;
    }
    return 0;
}
static void ms_load(int bin){
    if (ms_tab.bin == bin) return;
    ms_tab.bin = bin; ms_tab.n = 0;
    for (int i = 0; i < MS_KINDS; i++){
        char name[80]; snprintf(name, sizeof name, "milestones/bin_%02d/challenge_%d_%s", bin, bin, ms_kinds[i]);
        uint32_t key = h32(name), rec = ms_find(key, bin);
        if (rec){ ms_tab.m[ms_tab.n].kind = i; ms_tab.m[ms_tab.n].key = key; ms_tab.m[ms_tab.n].rec = rec; ms_tab.n++; }
    }
}
static int ms_state(int j){ return (int)(rd(ms_tab.m[j].rec + 4u) & 0xffu); }
/* the bin's evasion milestone: 0 none, else its state (4 met); *others: the bin's other milestones not met */
static int ms_evasion(int bin, int *others){
    ms_load(bin);
    int ev = 0, left = 0;
    for (int j = 0; j < ms_tab.n; j++){
        if (ms_tab.m[j].kind == MS_EVASION) ev = ms_state(j);
        else left += ms_state(j) != 4;
    }
    if (others) *others = left;
    return ev;
}
static void milestones_log(CPU *c, int bin){
    (void)c;
    ms_load(bin);
    char b[900]; int o = 0;
    for (int j = 0; j < ms_tab.n && o < 840; j++){
        uint32_t r = ms_tab.m[j].rec, fl = rd(r + 4u);
        o += snprintf(b + o, sizeof b - (size_t)o, " %s %s %.0f, best %.0f, %s;", ms_kinds[ms_tab.m[j].kind],
                      (fl >> 8) & 0xffu ? ">=" : "<=", (double)rdf(r + 8u), (double)rdf(r + 12u),
                      (fl & 0xffu) == 4 ? "met" : (fl & 0xffu) == 2 ? "open" : "state?");
    }
    fprintf(stderr, "[mwbot] f=%ld bin %d: %d milestones:%s\n", autotest_frame(), bin, ms_tab.n, ms_tab.n ? b : " none found");
}
static void pursuit_escape(CPU *c, float len, float bust, uint32_t pbounty){
    struct gates g; struct race r[MAX_RACES];
    float p[3] = { 0 }, kmh = 0;
    B.escape_t = mw_game_time(); B.escapes++;
    if (!read_gates(c, &g) || !mw_aid_where(c, p, &kmh)) return;
    /* boxed in where the last escape set it, hardly moving: that place is no way out (bin 8's race 5 place, sim
     * (-4432, 213, 1105): the car stood there at 1 km/h, the bust timer at 1.0, after three escapes of c5-s8t and
     * c5-s8u) */
    if (B.esc_recent[0] && hypotf(p[0] - B.esc_pos[0], p[2] - B.esc_pos[2]) < 60.0f && kmh < 10.0f &&
        B.esc_dest >= 0 && B.esc_dest < 32 && !((B.fell_mask >> B.esc_dest) & 1u)){
        B.fell_mask |= 1u << B.esc_dest;
        fprintf(stderr, "[mwbot] f=%ld t=%.1f pursuit escape %d: boxed in at race %d's place, where the last escape set "
                "the car: not there again\n", autotest_frame(), mw_game_time(), B.escapes, B.esc_dest);
    }
    int n = read_races(c, g.binobj, r, MAX_RACES, 0), best = -1;
    float bd = 0;
    /* not the places of this pursuit's last two escapes, nor one that left the car under the road: at bin 8 the car
     * went back and forth between two race places, boxed in at each within seconds of its arrival (bust timer 1.0
     * at 1 km/h; four of six escapes of one c5-s8t pursuit), and one of them had left it under the road */
    for (int pass = 0; pass < 2 && best < 0; pass++)
        for (int i = 0; i < n; i++){
            float d = hypotf(r[i].pos[0] - p[0], r[i].pos[2] - p[2]);
            int avoid = i + 1 == B.esc_recent[0] || i + 1 == B.esc_recent[1] || ((B.fell_mask >> i) & 1u);
            if (r[i].params && (pass || !avoid) && d > bd && (pass || d >= ESCAPE_FAR_M)){ bd = d; best = i; }
        }
    if (best < 0 || bd < ESCAPE_FAR_M){
        fprintf(stderr, "[mwbot] f=%ld t=%.1f pursuit escape %d: no race place %.0f m away (farthest %.0f m)\n",
                autotest_frame(), mw_game_time(), B.escapes, ESCAPE_FAR_M, bd);
        return;
    }
    fprintf(stderr, "[mwbot] f=%ld t=%.1f pursuit escape %d: chase %.0f s, bust timer %.1f, perp bounty %u, at %.0f %.0f %.0f "
            "%.0f km/h: to race %d's place, %.0f m away\n", autotest_frame(), mw_game_time(), B.escapes, len, bust, pbounty,
            p[0], p[1], p[2], kmh, best, bd);
    mw_aid_teleport(c, r[best].pos, r[best].dir);
    memcpy(B.esc_pos, r[best].pos, sizeof B.esc_pos); memcpy(B.esc_dir, r[best].dir, sizeof B.esc_dir); B.esc_check = 1;
    B.esc_recent[1] = B.esc_recent[0]; B.esc_recent[0] = best + 1; B.esc_dest = best;
}
/* A warp can leave the car under the road: escape 4 of c4-s9r set it at a race's place (height 28) and it stood at
 * -47 until the pursuit's end, the game's reset finding no place there (as GAME 13.7 has it for a warp right after a
 * load: a second warp stays on the road).  So FELL_M below the place asked, the car is set there again. */
#define FELL_M        10.0f
static int fell_below(CPU *c, const float want[3]){
    float p[3], kmh = 0;
    return mw_aid_where(c, p, &kmh) && hypotf(p[0] - want[0], p[2] - want[2]) < 30.0f && p[1] < want[1] - FELL_M;
}
/* NFSMW_BOT_SEARCH, what the bot does in the cops' search (state 2): unset or "rejoin", back to the chase (below);
 * "stay", nothing (the AI drives on and the game's own cooldown runs); "hide", the car set in the nearest hiding spot
 * that is on (the world map's icons of type 11, pursuit_mw.c) a second into the search, standing there until the chase
 * comes back (then the AI drives again) or the game ends the pursuit (TRACKER M4-16: hiding spots and the cooldown;
 * `pursuit watch` logs the meter and its parts). */
int mw_aid_icon_near(int type, const float at[3], float out[3], float *dist);   /* pursuit_mw.c */
#define HIDE_AFTER_S  1.0               /* game seconds of the search before the car is hidden */
#define HIDE_MAX      4                 /* times in one pursuit */
static struct { int hides, hidden; } H;
static int search_mode(void){             /* 0 rejoin, 1 stay, 2 hide */
    static int m = -1;
    if (m < 0){ const char *e = getenv("NFSMW_BOT_SEARCH"); m = e && !strcasecmp(e, "hide") ? 2 : e && !strcasecmp(e, "stay") ? 1 : 0; }
    return m;
}
static void pursuit_hide(CPU *c, uint32_t pst, double t){
    if (pst < 2 && H.hidden){           /* the cops found the car: the AI drives again */
        int was = H.hidden;
        H.hidden = 0;
        if (was == 2) return;           /* (it was not hidden: no spot) */
        mw_aid_autopilot(c, 1);
        fprintf(stderr, "[mwbot] f=%ld t=%.1f pursuit hide %d: the chase is back, the AI drives\n", autotest_frame(), t, H.hides);
        return;
    }
    if (pst != 2 || H.hidden || H.hides >= HIDE_MAX || t - B.search_t < HIDE_AFTER_S || mw_fe_input_top(0)) return;
    float p[3], s[3], kmh = 0, d = 0, dir[3] = { 1.0f, 0.0f, 0.0f };
    if (!mw_aid_where(c, p, &kmh)) return;
    int k = mw_aid_icon_near(11, p, s, &d);
    H.hides++;
    if (k < 0){                         /* none in GManager's list now: once a search, with the list's counts */
        fprintf(stderr, "[mwbot] f=%ld t=%.1f pursuit hide %d: no hiding spot on\n", autotest_frame(), t, H.hides);
        mw_aid_icons(c, -1);
        H.hidden = 2;                   /* not again until the chase is back */
        return;
    }
    H.hidden = 1;
    fprintf(stderr, "[mwbot] f=%ld t=%.1f pursuit hide %d: the search %.1f s on; to hiding spot (icon %d) at %.0f %.0f %.0f, "
            "%.0f m away\n", autotest_frame(), t, H.hides, t - B.search_t, k, s[0], s[1], s[2], d);
    mw_aid_teleport(c, s, dir);
    mw_aid_autopilot(c, AP_STAND);
}
/* or thrown into the air: escape 4 of a c5-s8w pursuit set the car at bin 8's race 1 place (height 226) and 2.5 s on
 * it was 100 m away at height 716, then fell through the world (-9,000 at 1,080 km/h), and the rejoins took it back
 * to a trail point in the air.  A place the car is thrown from is not used again; the trail keeps only places at
 * road heights (TRAIL_Y_*) and speeds, and a rejoin goes only there. */
#define THROWN_M      40.0f
#define TRAIL_Y_MIN   -30.0f
#define TRAIL_Y_MAX   400.0f
static int thrown_up(CPU *c, const float want[3]){
    float p[3], kmh = 0;
    return mw_aid_where(c, p, &kmh) && hypotf(p[0] - want[0], p[2] - want[2]) < 400.0f && p[1] > want[1] + THROWN_M;
}
static int road_place(const float p[3], float kmh){ return p[1] > TRAIL_Y_MIN && p[1] < TRAIL_Y_MAX && kmh < 400.0f; }
/* A pursuit is forced only on a car the AI drives: one standing (the AI parks the player's car in a hiding spot in the
 * cops' search and does not drive out after the evade: at sim -957 122 1847 in c5-s5c and c6-b5s) gets a forced
 * pursuit whose one cop never sees it, a 12 s chase and a search to the evade (twice in a row in c6-b5s, 8 rejoins
 * each to no cop).  So a car under UNSTICK_KMH when the pursuit is due is set at the place of the bin's race farthest
 * from it, and the pursuit forced UNSTICK_S later (a few times, then forced as it stands).  1: not yet. */
#define UNSTICK_KMH   30.0f
#define UNSTICK_S     8.0
#define UNSTICK_MAX   3
static int unstick(CPU *c, double t){
    float p[3] = { 0 }, kmh = 0;
    if (!mw_aid_where(c, p, &kmh) || kmh >= UNSTICK_KMH || B.unsticks >= UNSTICK_MAX) return 0;
    struct gates g; struct race r[MAX_RACES];
    if (!read_gates(c, &g)) return 0;
    int n = read_races(c, g.binobj, r, MAX_RACES, 0), best = -1;
    float bd = 0;
    for (int i = 0; i < n; i++){
        float d = hypotf(r[i].pos[0] - p[0], r[i].pos[2] - p[2]);
        if (r[i].params && !((B.fell_mask >> i) & 1u) && d > bd){ bd = d; best = i; }
    }
    if (best < 0) return 0;
    B.unsticks++;
    fprintf(stderr, "[mwbot] f=%ld t=%.1f the car stands (%.0f km/h at %.0f %.0f %.0f) when the pursuit is due: to race %d's place, "
            "%.0f m away, the pursuit %.0f s on (%d)\n", autotest_frame(), t, kmh, p[0], p[1], p[2], best, bd, UNSTICK_S, B.unsticks);
    mw_aid_teleport(c, r[best].pos, r[best].dir);
    mw_aid_autopilot(c, 1);
    B.t0 = t - (PURSUIT_NATURAL_S > UNSTICK_S ? PURSUIT_NATURAL_S - UNSTICK_S : 0.0);
    if (B.chase_s > 0.0) B.t0 = t - (CHASE_NATURAL_S > UNSTICK_S ? CHASE_NATURAL_S - UNSTICK_S : 0.0) ;
    return 1;
}
static void pursuit_frame(CPU *c){
    uint32_t perp = 0, po = mw_aid_pursuit(c, &perp);
    double t = mw_game_time();
    long f = autotest_frame();
    if (t < B.t0) B.t0 = t;             /* the world was loaded again (a bust): its clock starts again */
    if (!B.raced){                       /* no pursuit yet: the AI drives about until the cops start one */
        if (B.tries == 0 && game_flow() == g_game.flow_race && !mw_fe_input_top(0)){
            mw_aid_autopilot(c, B.stand ? AP_STAND : 1); B.tries = 1; B.bounty0 = B.g.bounty;   /* bot bust: it stands */
        }
        if (po){
            B.raced = 1; B.pursuit = po; B.next_log = t; B.escapes = 0; B.escape_t = -1e9; B.unsticks = 0; B.rejoin_stand = 0;
            B.esc_recent[0] = B.esc_recent[1] = 0; B.hides = 0; B.hidden = 0;
            B.trail_n = 0; B.rejoins = 0; B.last_pst = 0; B.trail_t = -1e9; B.search_t = t;
            H.hides = 0; H.hidden = 0;
            fprintf(stderr, "[mwbot] f=%ld t=%.1f pursuit %08x began%s: heat %.2f range %.2f-%.2f cops %u\n", f, t, po,
                    B.forced ? " (forced)" : " (by itself)", rdf(perp + PERP_HEAT), rdf(po + 0x9cu), rdf(po + 0xa0u),
                    rd(po + PURSUIT_COPS));
        } else if (B.tries == 1 && t - B.t0 >= (B.chase_s > 0.0 || B.stand ? CHASE_NATURAL_S : PURSUIT_NATURAL_S) &&
                   free_roam(c) && !(B.stand ? 0 : unstick(c, t))){
            /* at the bin's top heat (the pursuit holds it in the bin's range anyway): forced at heat 1 the pursuits of
             * bin 14 mostly ended within 12 s for 100 bounty, while the one at heat 1.96 ran 176 s for 7,700 (bot run
             * of 2026-10-30) */
            uint32_t heat = (uint32_t)bin_max_heat(B.g.bin);
            const char *fh = getenv("NFSMW_BOT_FHEAT");   /* another heat (1: the game's escalation raises it) */
            if (fh && atoi(fh) > 0) heat = (uint32_t)atoi(fh);
            call1(c, FN_FORCE_PURSUIT, 0, heat);
            B.forced = 1; B.tries = 2;
            fprintf(stderr, "[mwbot] f=%ld t=%.1f no pursuit after %.0f s of driving: Game.ForcePursuitStart(%u)\n", f, t,
                    t - B.t0, heat);
        }
        return;
    }
    if (po){
        B.plen = rdf(po + PURSUIT_TIME);
        B.end_f = 0;
        if (B.last_t > 0.0 && t > B.last_t && t - B.last_t < 1.0) B.chase_acc += t - B.last_t;
        B.last_t = t;
        /* The AI drives the whole pursuit.  What was tried to make chases longer or to save a boxed-in car from the bust,
         * and dropped (bot runs of 2026-10-30): letting the car stand when the cops lost it (state +0x218 == 2, the
         * cops' search) did not bring them back, the cooldown ran out, and cops that did come busted the standing car;
         * the game's RESET when the bust timer (+0xDC) passed 1.5 s at walking speed put the car on the road beside
         * the same cops, the timer ran on (8.6 four seconds later) and the next reset found no place; every pursuit of
         * that run ended busted, three busts impounded the only car, and the career ended (GameOver.fng).  With the AI
         * alone two pursuits of about ten ended busted in Sonny's bin, three of four in the next, each within 6 s of a
         * wedge reset; a reset at once when the car was down to walking speed with a cop at it (bust timer 1.0) was
         * followed by the bust too (the timer at 4.9 a second later, twice): a reset in a pursuit is a bust, so there is
         * none (career_mw.c's wedge watch leaves a pursuit alone).
         * What the AI cannot do is lose the cops: at the top heat of bin 13 a chase ran 550 s with five cops on the car
         * at up to 236 km/h, 28,925 bounty on it, and ended busted (bot run of 2026-10-30), and a bust keeps none of
         * the chase's bounty or milestones.  So a chase that has run escape_len (ESCAPE_LEN, more in a bin with a
         * pursuit_length milestone), or whose bust timer is ESCAPE_BUST on (the car boxed in), is broken off the way the bot reaches a race: the car set at the place of one of the
         * bin's races at least ESCAPE_FAR_M away (the farthest), the AI driving on from there.  The cops lose it, their
         * search runs its cooldown and the game ends the pursuit as evaded, banking the bounty and the milestones
         * itself (a bin-13 chase broken off once at 129 s: 39,000 bounty and its three milestones banked).  Not in the
         * cops' search (state 2) nor once busted (3): in state 1, a chase with one cop on the car, a car standing with
         * the bust timer at 2.8 was busted when only state 0 was broken off. */
        float bust = rdf(po + PURSUIT_BUST_T), len = rdf(po + PURSUIT_TIME);
        uint32_t pst = rd(po + PURSUIT_STATE);
        if (B.esc_check && t - B.escape_t >= 1.5 && !mw_fe_input_top(0)){   /* the escape's warp: on the road? */
            int up = 0;
            if ((fell_below(c, B.esc_pos) || (up = thrown_up(c, B.esc_pos))) && B.esc_check < 3){
                fprintf(stderr, "[mwbot] f=%ld t=%.1f pursuit escape %d: the car is %s, set there again\n", f, t,
                        B.escapes, up ? "thrown into the air" : "under the road");
                if (B.esc_dest >= 0 && B.esc_dest < 32) B.fell_mask |= 1u << B.esc_dest;   /* not there again */
                mw_aid_teleport(c, B.esc_pos, B.esc_dir); B.escape_t = t; B.esc_check++;
            } else B.esc_check = 0;
        }
        float elen = B.quick ? QUICK_LEN : escape_len(c, B.g.bin);   /* a quick pursuit: broken off at once */
        if (!B.stand && pst < 2 && B.escapes < ESCAPE_MAX && t - B.escape_t >= (bust >= ESCAPE_BUST ? 10.0 : ESCAPE_GAP_S) &&
            (bust >= ESCAPE_BUST || len >= elen) && !mw_fe_input_top(0)){
            pursuit_escape(c, len, bust, rd(perp + 0x20u));
            B.trail_n = 0;                  /* no way back to where the car was boxed in */
        }
        /* The other way round: a chase lost too early.  Forced at the top heat of bin 12, chase after chase ran 12 s
         * (one cop, left behind at once), then 133 s of the cops' search to the evade, for 1,000 bounty each and no
         * milestone (bot run of 2026-10-30).  So while the chase is under escape_len, a search that has run REJOIN_S
         * takes the car back to where the chase was REJOIN_BACK seconds before it began (the trail below, kept while
         * the cops are on the car), heading the way it went, the AI driving on: the cops searching there see it again
         * and the chase goes on, as a player who doubles back would have it. */
        if (pst < 2 && !mw_fe_input_top(0) && t - B.trail_t >= 1.0){
            float kmh = 0, p[3];
            if (mw_aid_where(c, p, &kmh) && road_place(p, kmh)){
                memcpy(B.trail[B.trail_n % TRAIL_N], p, sizeof p); B.trail_n++; B.trail_t = t;
            }
        }
        if (pst == 2 && B.last_pst != 2) B.search_t = t;
        B.last_pst = (int)pst;
        if (!B.stand && search_mode() == 2) pursuit_hide(c, pst, t);
        /* Once the chase has run escape_len, the pursuit is to end: in the cops' search the car is set in the nearest
         * hiding spot and stands there (the AI let go) until the game ends the pursuit as evaded, or the chase comes
         * back (then the AI drives again).  At bin 8 a chase ran 804 s through 23 escapes to race places, each found
         * by the cops again within 20 s (c5-s8x).  The hiding spots are GManager's icons of type 11 (HIDING_SPOT_%d,
         * 34 of them, listed only in the cops' search; pursuit_mw.c), in the world's axes; a quick pursuit's (a bin's
         * open "escape within" milestone) at once.  Only with NFSMW_BOT_SEARCH unset (rejoin): "stay" leaves the
         * search alone and "hide" hides at once (pursuit_hide). */
        if (!B.stand && search_mode() == 0 && pst == 2 && !B.hidden && B.hides < ESC_HIDE_MAX && (len >= elen || B.quick) &&
            t - B.search_t >= 1.0 && !mw_fe_input_top(0)){
            float here[3], kmh = 0, w[3], best[3] = { 0 }, bd = 1e30f;
            if (mw_aid_where(c, here, &kmh))
                for (int k = 0; k < 64 && mw_aid_icon_place(ICON_HIDING, k, w); k++){
                    float s[3] = { -w[1], w[2], w[0] }, d = hypotf(s[0] - here[0], s[2] - here[2]);
                    if (d < bd){ bd = d; memcpy(best, s, sizeof best); }
                }
            B.hides++;
            if (bd < 3000.0f){
                float dir[3] = { 1.0f, 0.0f, 0.0f };
                fprintf(stderr, "[mwbot] f=%ld t=%.1f pursuit hidden %d: chase %.0f s, the search %.0f s on; to the hiding spot at "
                        "%.0f %.0f %.0f, %.0f m away, standing\n", f, t, B.hides, len, t - B.search_t, best[0], best[1], best[2], bd);
                mw_aid_teleport(c, best, dir);
                mw_aid_autopilot(c, AP_STAND);
                B.hidden = 1;
            } else fprintf(stderr, "[mwbot] f=%ld t=%.1f pursuit: no hiding spot within 3 km (nearest %.0f m)\n", f, t, bd);
        }
        if (B.hidden && pst < 2){          /* the chase came back: drive */
            fprintf(stderr, "[mwbot] f=%ld t=%.1f pursuit: the cops found the hidden car, the AI drives again\n", f, t);
            mw_aid_autopilot(c, 1);
            B.hidden = 0;
        }
        if (!B.stand && search_mode() == 0 && !B.quick && !B.rejoin_stand && pst == 2 && len < elen && B.rejoins < REJOIN_MAX &&
            t - B.search_t >= REJOIN_S && B.trail_n > REJOIN_BACK && !mw_fe_input_top(0)){
            const float *a = B.trail[(B.trail_n - REJOIN_BACK - 1) % TRAIL_N], *b = B.trail[(B.trail_n - REJOIN_BACK) % TRAIL_N];
            float d[3] = { b[0] - a[0], 0.0f, b[2] - a[2] }, m = hypotf(d[0], d[2]);
            if (m > 0.5f){ d[0] /= m; d[2] /= m; } else { d[0] = 1.0f; d[2] = 0.0f; }
            /* the trail is kept: a search still on REJOIN_S later (the cops gone elsewhere) takes the car back again.
             * From the second time on, the car goes to where the cops search (the pursuit's search position, +0x118,
             * simulation space): at bin 8 the trail's place found no cop eight times in a row, pursuit after pursuit
             * (12 to 16 s chases, 8 rejoins each, evaded for 5,000 bounty: c5-pr8, c5-s8v) */
            float sp[3] = { rdf(po + PURSUIT_SEARCH), rdf(po + PURSUIT_SEARCH + 4u), rdf(po + PURSUIT_SEARCH + 8u) }, here[3], kmh = 0;
            int use_search = B.rejoins >= 1 && mw_aid_where(c, here, &kmh) && (sp[0] != 0.0f || sp[2] != 0.0f) &&
                             hypotf(sp[0] - here[0], sp[2] - here[2]) < 3000.0f && fabsf(sp[1] - a[1]) < 200.0f &&
                             sp[1] > TRAIL_Y_MIN && sp[1] < TRAIL_Y_MAX;
            const float *to = use_search ? sp : a;
            B.rejoins++;
            fprintf(stderr, "[mwbot] f=%ld t=%.1f pursuit rejoined %d: chase %.0f s, the search %.0f s on; back to %.0f %.0f %.0f%s\n",
                    f, t, B.rejoins, len, t - B.search_t, to[0], to[1], to[2], use_search ? " (the cops' search position)" : "");
            B.search_t = t;
            mw_aid_teleport(c, to, d);
            /* and stands there, the AI let go, until the cops see it (the chase again) or REJOIN_STAND_S pass: driven
             * by the AI from there, the car went straight to a hiding spot (the game's AI hides the player's car in
             * the cops' search: it parked at sim -957 122 1847 pursuit after pursuit in c6-b5s, every rejoin wasted) */
            mw_aid_autopilot(c, AP_STAND);
            B.rejoin_stand = 1; B.rejoin_t = t;
        }
        if (B.rejoin_stand && (pst < 2 || t - B.rejoin_t >= REJOIN_STAND_S || bust >= ESCAPE_BUST)){
            fprintf(stderr, "[mwbot] f=%ld t=%.1f pursuit: %s after %.0f s standing where the rejoin set it; the AI drives\n", f, t,
                    pst < 2 ? "the cops see the car" : bust >= ESCAPE_BUST ? "a cop at the car" : "no cop came", t - B.rejoin_t);
            mw_aid_autopilot(c, 1);
            B.rejoin_stand = 0;
        }
        if (t >= B.next_log){
            static double every = -1.0;     /* NFSMW_BOT_PLOG=S: a line every S game seconds (the car's track), else 15 */
            if (every < 0.0){ const char *e = getenv("NFSMW_BOT_PLOG"); every = e && atof(e) > 0.0 ? atof(e) : 15.0; }
            B.next_log = t + every;
            float kmh = 0, p[3] = { 0 }; mw_aid_where(c, p, &kmh);
            fprintf(stderr, "[mwbot] f=%ld t=%.1f pursuit %.0f s: state %u heat %.2f cops %u bust %.1f perp bounty %u at %.0f "
                    "%.0f %.0f %.0f km/h\n", f, t, rdf(po + PURSUIT_TIME), rd(po + PURSUIT_STATE), rdf(perp + PERP_HEAT),
                    rd(po + PURSUIT_COPS),
                    rdf(po + PURSUIT_BUST_T), rd(perp + 0x20u), p[0], p[1], p[2], kmh);
        }
        return;
    }
    B.last_t = 0.0;
    /* The pursuit's end: its screens first.  A bust's come late: the pursuit object goes, free roam goes on for a few
     * seconds, then the arrest, the infractions sheet (Infractions.fng: Enter pays the fines, a strike on the car), the
     * car lot's choice (Car_Select.fng) and the safehouse.  A pursuit forced in those seconds took the bust's place:
     * no sheet, no strike (bot chase run of 2026-10-30, the first of two busts), so after a bust the bot waits for
     * the sheet, BUST_FLOW_MAX frames at most. */
    if (!B.end_f){ B.end_f = f; B.end_kind = mw_aid_last_end(); B.sheet_seen = 0; }
    if (B.end_kind == 1 && !B.sheet_seen && f - B.end_f < BUST_FLOW_MAX) return;
    if (!settled(c)) return;
    struct gates g; read_gates(c, &g);
    int end = B.end_kind;
    B.end_f = 0;
    B.pursuits++; B.busts += end == 1; B.evades += end == 2;
    fprintf(stderr, "[mwbot] f=%ld t=%.1f pursuit over (%.0f s long, %.0f s of game time in all, %d escapes, %d rejoined): "
            "%s; bounty %d -> %d, milestones %d -> %d; pursuits %d, busted %d, evaded %d\n", f, t, B.plen, t - B.t0,
            B.escapes, B.rejoins, end == 1 ? "busted" : end == 2 ? "evaded" : "ended (no bust or evade seen)", B.bounty0,
            g.bounty, B.g.ms, g.ms, B.pursuits, B.busts, B.evades);
    milestones_log(c, g.bin);
    if (B.quick){
        fprintf(stderr, "[mwbot] f=%ld t=%.1f quick pursuit %d of the bin (escape within %.0f s): chase %.0f s, %s; milestones %d -> "
                "%d%s\n", f, t, B.quick_tries, (double)evasion_goal(c, g.bin), B.plen, end == 1 ? "busted" : end == 2 ? "evaded" :
                "ended", B.g.ms, g.ms, g.ms > B.g.ms && end == 2 ? ": the escape-within milestone taken as met" : "");
        if (g.ms > B.g.ms && end == 2) B.quick_ok = 1;
        B.quick = 0;
    }
    mw_aid_rapsheet(c, end == 1 ? "after a bust" : end == 2 ? "after an evade" : "after a pursuit");
    mw_aid_icons(c, 12);                /* the world map's icons by type, the pursuit breakers' one by one */
    mw_aid_pools(c, 1);                 /* the memory pools, peaks since the first pursuit's end (M4-48, M4-35) */
    mw_aid_copcars(end == 1 ? "busted" : end == 2 ? "evaded" : "ended");   /* the heat's cop units (M4-16) */
    mw_aid_autopilot(c, AP_STAND);
    B.raced = 0; B.pursuit = 0; B.forced = 0; B.rejoin_stand = 0;
    if (B.bust_n){                      /* bot bust: the next pursuit, until the busts asked for */
        uint32_t cur = rd(profile() + 0xa8u), cr = career_rec(profile(), cur);
        fprintf(stderr, "[mwbot] f=%ld t=%.1f bust: %d of %d busts; the current car %#x: strikes %d of %d, %s; Out of Jail "
                "markers %d (spent %d), impounds %d\n", f, t, B.busts, B.bust_n, cur, cr ? MR8(cr + 3u) : -1,
                cr ? MR8(cr + 2u) : -1, cr && !MR8(cr + 4u) ? "drivable" : "impounded", B.oojail, B.jail_spent, B.impounds);
        if (B.busts >= B.bust_n){ set_state(B_DONE, "the busts asked for"); return; }
        B.g = g; B.tries = 0; B.t0 = t;
        return;
    }
    if (B.chase_s > 0.0){               /* bot chase: the next pursuit, until they have lasted chase_s in all */
        fprintf(stderr, "[mwbot] f=%ld t=%.1f chase: %.0f s of %.0f s in pursuits\n", f, t, B.chase_acc, B.chase_s);
        if (B.chase_acc >= B.chase_s){ set_state(B_DONE, "the chase's time is reached"); return; }
        B.g = g; B.tries = 0; B.t0 = t;
        return;
    }
    if (B.once && !B.after){ set_state(B_DONE, "the probe's pursuit is over"); return; }
    int back = B.after ? B.after : B_ROAM;
    B.after = 0;
    set_state(back, "pursuit over");
}

/* A car off the world (below or far above any road): the game's own reset did not catch it.  After the throw of a
 * c5-s8w escape (thrown_up) the car fell for good, 150 km down at 1,080 km/h through four more pursuits that the
 * cops could only lose, their bounty banked by the game.  So in the world, out of a race, a car outside road heights
 * is set at the place of the bin's race nearest it (in a race the game's RESET, as the wedge watch does it). */
#define OFFWORLD_Y_MIN  -500.0f         /* not -100: between a rival's challenge accepted and its race the car stood at
                                           -101 at 309 km/h and was set at a race place (c5-b6a; the race came all the
                                           same) */
#define OFFWORLD_Y_MAX  1500.0f
static void off_world(CPU *c){
    static double last;
    float p[3], kmh = 0;
    double t = mw_game_time();
    if (B.st == B_ENGAGE || (B.st == B_RACE && !racing(c))) return;   /* a race's load and set-up, or its end: the game's */
    if (game_flow() != g_game.flow_race || mw_fe_input_top(0) || !mw_aid_where(c, p, &kmh)) return;
    if (p[1] > OFFWORLD_Y_MIN && p[1] < OFFWORLD_Y_MAX) return;
    if (t >= last && t - last < 3.0) return;
    last = t;
    if (racing(c)){
        int ok = mw_reset_car(c);
        fprintf(stderr, "[mwbot] f=%ld t=%.1f the car is off the world (at %.0f %.0f %.0f, %.0f km/h): the game's reset %s\n",
                autotest_frame(), t, p[0], p[1], p[2], kmh, ok > 0 ? "done" : "found no place");
        return;
    }
    struct gates g; struct race r[MAX_RACES];
    if (!read_gates(c, &g)) return;
    int n = read_races(c, g.binobj, r, MAX_RACES, 0), best = -1;
    float bd = 1e30f;
    for (int i = 0; i < n; i++){
        float d = hypotf(r[i].pos[0] - p[0], r[i].pos[2] - p[2]);
        if (r[i].params && !((B.fell_mask >> i) & 1u) && d < bd){ bd = d; best = i; }
    }
    if (best < 0) return;
    fprintf(stderr, "[mwbot] f=%ld t=%.1f the car is off the world (at %.0f %.0f %.0f, %.0f km/h): to race %d's place, %.0f m away\n",
            autotest_frame(), t, p[0], p[1], p[2], kmh, best, bd);
    mw_aid_teleport(c, r[best].pos, r[best].dir);
}

void mw_bot_frame(CPU *c){
    if (B.st == B_OFF || B.st == B_DONE) return;
    long f = autotest_frame();
    off_world(c);
    if (f - B.mk_f >= 60 || f < B.mk_f){      /* the Out of Jail markers, for the infractions sheet (mw_bot_present) */
        uint32_t a[2] = { MK_OUT_OF_JAIL, 0 };
        B.mk_f = f;
        B.oojail = (int)call(c, FN_MARKERS, G_MARKERS, 2, a);
    }
    switch (B.st){
    case B_ROAM:
        if (pursuit_on(c, B_ROAM)) break;
        if (game_flow() == g_game.flow_race && racing(c)){ set_state(B_RACE, "a race began (the rival's next)"); break; }
        if (settled(c)) decide(c);
        break;
    case B_GOTO: {                      /* at the start: Enter until the engage dialog comes */
        if (pursuit_on(c, B_ROAM)) break;
        if (mw_fe_input_top(0)) break;  /* a screen (the dialog itself: B_ENGAGE takes it in mw_bot_present) */
        if (B.via){                     /* settled at the other place (go_to): now to the start */
            if (!settled(c)) break;
            B.via = 0; B.free_since = 0;
            mw_aid_teleport(c, B.tpos, B.tdir);
            break;
        }
        if (!settled(c) || f - B.last_tap < ENGAGE_EVERY) break;
        float p[3], kmh = 0;            /* not there (the warp undone, or a menu's reload put it back, or under the
                                           road): again */
        if (mw_aid_where(c, p, &kmh) && (hypotf(p[0] - B.tpos[0], p[2] - B.tpos[2]) > GOTO_NEAR_M || fell_below(c, B.tpos))){
            if (B.warps >= 3){ if (B.race >= 0) B.skips[B.race]++; set_state(B_ROAM, "the car does not stay at the start"); break; }
            fprintf(stderr, "[mwbot] f=%ld the car is at %.1f %.1f %.1f, %.0f m from the start: warped again\n", f, p[0], p[1],
                    p[2], hypotf(p[0] - B.tpos[0], p[2] - B.tpos[2]));
            mw_aid_teleport(c, B.tpos, B.tdir); B.warps++; B.free_since = 0;
            break;
        }
        if (B.tries >= ENGAGE_TRIES){
            if (B.race >= 0) B.skips[B.race]++; else B.boss_engage_fail = 1;
            fprintf(stderr, "[mwbot] f=%ld race %d: no engage dialog after %d Enters (skip %d)\n", f, B.race, B.tries,
                    B.race >= 0 ? B.skips[B.race] : B.boss_tries);
            set_state(B_ROAM, "not engaged");
            break;
        }
        autotest_tap(DIK_RETURN, 6); B.last_tap = f; B.tries++;
        break; }
    case B_ENGAGE: {                    /* accepted: the race's load, its start, the AI from its GO */
        if (game_flow() == g_game.flow_race && racing(c)){
            set_state(B_RACE, "the race is on");
            break;
        }
        if (f - B.f0 > RACE_START_MAX){ if (B.race >= 0) B.skips[B.race]++; set_state(B_ROAM, "the race did not start"); }
        break; }
    case B_RACE:                        /* the race's end: its result screen (the winner's or the loser's), then free roam */
        if (!B.fin_f && !racing(c)) B.fin_f = f;
        if (!B.fin_f) break;
        if (!B.post_seen && f - B.fin_f < POST_WAIT) break;
        /* free roam again, or cops on the car (a pursuit that began in the race goes on after it: the AI must drive at
         * once, a car left standing was busted a minute on, bot run of 2026-10-30) */
        if (!settled(c) && !(game_flow() == g_game.flow_race && !mw_fe_input_top(0) && mw_aid_pursuit(c, 0))) break;
        {
            struct gates g; struct race r[MAX_RACES];
            read_gates(c, &g);
            int n = read_races(c, g.binobj, r, MAX_RACES, 0);
            fprintf(stderr, "[mwbot] f=%ld t=%.1f race %d over: %s; races won %d -> %d, bounty %d -> %d\n", f,
                    mw_game_time(), B.race, B.race < 0 ? "(the rival's)" : B.race < n && (r[B.race].flags & 2u) ? "completed" : "not completed", B.g.won,
                    g.won, B.g.bounty, g.bounty);
            mw_aid_autopilot(c, AP_STAND);
            if (B.race < 0) B.boss_ended++;
            if (B.once){ set_state(B_DONE, "the probe's race is over"); break; }
            set_state(B_ROAM, "race over");
        }
        break;
    case B_PURSUIT: pursuit_frame(c); break;
    case B_SHOP: shop_frame(c); break;
    case B_CAR:                        /* another car: Jump to Safe House, Car Select, Resume Free Roam (mw_bot_present) */
        if (pursuit_on(c, B_CAR)) break;
        if (B.car_swapped){ if (settled(c)) set_state(B.once ? B_DONE : B_ROAM, "the car changed"); break; }
        if (f - B.f0 > CAR_SWAP_MAX){ B.want_car = 0; set_state(B_ROAM, "the car was not changed"); break; }
        if (settled(c) && f - B.last_tap >= 90){ autotest_tap(DIK_ESCAPE, 6); B.last_tap = f; }
        break;
    case B_SAVE:                       /* from free roam: the pause menu (the screens do the rest, mw_bot_present) */
        if (pursuit_on(c, B_SAVE)) break;
        if (settled(c) && f - B.last_tap >= 90){ autotest_tap(DIK_ESCAPE, 6); B.last_tap = f; }
        break;
    }
}

/* ---- the screens: answered each Present (the main loop's anchor does not run behind a loading screen) ----
 * Menus are told by their package, a label they draw and the highlighted object (fe_mw.c's names):
 *   the safehouse menu, MainMenu_Sub.fng drawing 'safe house' 0xE596C4A3: items 0x4B4E388A on, Resume Free Roam
 *     first; Save the fourth (0x4B4E388D), the fifth when Rap Sheet (0xAC22F27E) is in the menu (0x532220);
 *   Dialog.fng (leave the safehouse?): No 0xB8A7C6CD highlighted, Yes 0xB8A7C6CC to its left;
 *   Pause_Main.fng in free roam: Resume Free Roam 0x4B4E388A, Jump to Safe House 0x4B4E388B (bin 15 and on);
 *   InGameDialog.fng: Cancel 0xB8A7C6CD highlighted, OK 0xB8A7C6CC to its left;
 *   MC_Main.fng's save questions: No 0xB8A7C6CC highlighted, Yes 0xB8A7C6CD to its left; then 'Save complete.'. */
int mw_fe_match(const char *name);                            /* fe_mw.c */
#define DIK_RIGHT  0xcdu
#define F_ITEM1    0x4b4e388au
#define F_ITEM2    0x4b4e388bu
#define F_NO_DLG   0xb8a7c6cdu          /* Dialog.fng's No, InGameDialog.fng's Cancel, MC_Main.fng's Yes */
#define F_YES_DLG  0xb8a7c6ccu          /* Dialog.fng's Yes, InGameDialog.fng's OK, MC_Main.fng's No */
static struct { char name[64]; uint32_t focus, save_last; long since, last, jail_since, cs_since; int taps, marker_enter, save_same, shown, mk_open, mk_cash, mk_cars, mk_taken, mk_moves, mk_held[MK_TYPES], cs_open; } S;
static void save_begin(void){ S.save_same = 0; S.save_last = 0; }
/* the screens whose strings the bot logs once they settle (the rewards and results it does not read otherwise) */
static int shows_logged(const char *nm){
    static const char *const k[] = { "SafehouseMarkers.fng", "SafeHouseRivalChallenge.fng", "PostRace_Results.fng",
        "PostRace_MilestoneRewards.fng", "PostRace_Pursuit.fng", "InGameRivalChallenge.fng", "Infractions.fng",
        "Car_Select.fng", "GameOver.fng", "SafeHouseRivalBio.fng", "ShoppingCart.fng", "InGameDialog.fng",
        "SafeHouseRegionUnlock.fng", "Dialog.fng", 0 };
    for (int i = 0; k[i]; i++) if (!strcasecmp(nm, k[i])) return 1;
    return 0;
}
void mw_bot_present(void){
    if (B.st == B_OFF || B.st == B_DONE) return;
    uint32_t focus = 0;
    const char *nm = mw_fe_input_top(&focus);
    long f = autotest_frame();
    if (!nm){ S.name[0] = 0; return; }
    if (strcmp(nm, S.name) || focus != S.focus){
        if (strcmp(nm, S.name)){
            /* the markers' cards show no names (pictures): what they gave is told by the career's cash and cars
             * before the screen and after it (a pink slip is a car more; a part, an impound box or a get out of jail
             * card changes neither) */
            uint32_t prof = profile();
            if (!strcasecmp(nm, "SafehouseMarkers.fng")){
                if (!S.mk_open){
                    S.mk_open = 1; S.mk_cash = (int)rd(prof + PROF_CASH); S.mk_cars = career_cars(prof);
                    S.mk_taken = 0; S.mk_moves = 0;
                    markers_held(S.mk_held);
                }
            } else if (S.mk_open){
                /* and the markers held before and after, by type (the parts' markers are the types below 0x11) */
                int now[MK_TYPES], o = 0; char b[300];
                markers_held(now);
                for (int t = 0; t < MK_TYPES && o < 260; t++)
                    if (now[t] != S.mk_held[t])
                        o += snprintf(b + o, sizeof b - (size_t)o, " %s %#x %d -> %d", marker_name(t), t, S.mk_held[t], now[t]);
                S.mk_open = 0;
                fprintf(stderr, "[mwbot] f=%ld markers taken: cash %d -> %d, cars %d -> %d; markers held:%s\n", f, S.mk_cash,
                        (int)rd(prof + PROF_CASH), S.mk_cars, career_cars(prof), o ? b : " no change");
            }
            S.taps = 0; S.since = f; S.shown = 0;
            if (strcasecmp(nm, "Car_Select.fng") && strcasecmp(nm, "Dialog.fng")){ B.cs_k = 0; B.cs_retry = 0; B.cs_dlg = 0; }
            else if (!strcasecmp(nm, "Dialog.fng") && B.cs_k) B.cs_dlg = 1;   /* a Dialog after the Enter on the list */
        }
        fprintf(stderr, "[mwbot] f=%ld screen %s @%08x (%s)\n", f, nm, focus, st_name[B.st]);
        snprintf(S.name, sizeof S.name, "%s", nm); S.focus = focus;
    }
    if (f - S.last < 45 || f - S.since < 30) return;
    if (!S.shown && shows_logged(nm)){
        char sh[1600]; mw_fe_shows(sh, sizeof sh);
        fprintf(stderr, "[mwbot] f=%ld %s shows%s\n", f, nm, sh);
        S.shown = 1;
    }
    unsigned key = DIK_RETURN; const char *what = "Enter";
    int save = B.st == B_SAVE;
    if (!strcasecmp(nm, "GameOver.fng") || !strcasecmp(nm, "MC_List.fng") ||
        (!strcasecmp(nm, "MainMenu_Sub.fng") && !mw_fe_match("MainMenu_Sub.fng:e596c4a3"))){
        /* the career is over (the only car impounded after its third bust), or a career menu the bot did not ask for:
         * no key from the bot there (an Enter could start a new career or load another alias) */
        fprintf(stderr, "[mwbot] f=%ld %s @%08x: the bot stops (%s)\n", f, nm, focus,
                !strcasecmp(nm, "GameOver.fng") ? "game over: the career car was impounded" : "a career menu");
        set_state(B_DONE, !strcasecmp(nm, "GameOver.fng") ? "game over" : "a career menu came");
        return;
    }
    if (!strcasecmp(nm, "InGameRivalChallenge.fng") && B.st == B_GOTO){
        /* the rival's challenge at its start: Enter accepts it (its handler 0x551790 on the accept message, 0x406415E3,
         * when the challenge may start: +0x2D), and the game takes the car to the race; Escape (0x911AB364) declines */
        autotest_tap(DIK_RETURN, 6); S.last = f; S.taps++;
        fprintf(stderr, "[mwbot] f=%ld %s @%08x: Enter, the rival's challenge accepted\n", f, nm, focus);
        set_state(B_ENGAGE, "the rival's challenge accepted");
        return;
    }
    if (!strcasecmp(nm, "EngageEventDialog.fng")){
        if (B.st == B_GOTO){            /* the bot's: Accept is to the left of the default Decline */
            if (!B.focus0_set){ B.focus0 = focus; B.focus0_set = 1; }
            if (focus == B.focus0){ key = DIK_LEFT; what = "LEFT to Accept"; }
            else {
                autotest_tap(DIK_RETURN, 6); S.last = f; S.taps++;
                fprintf(stderr, "[mwbot] f=%ld %s @%08x: Enter on Accept\n", f, nm, focus);
                B.focus0_set = 0;
                set_state(B_ENGAGE, "accepted");
                return;
            }
        } else what = "Enter on Decline (not the bot's)";
    }
    else if (!strcasecmp(nm, "SMS_Message.fng") || !strcasecmp(nm, "SMS_MailBoxes.fng")){ key = DIK_ESCAPE; what = "Escape"; }
    else if (!strcasecmp(nm, "Infractions.fng")){
        /* after a bust: the default button (0xB8A7C6CD) pays the fines from the cash and puts a strike on the car
         * (the screen's handler 0x5A162E: strikes +3 of the career record up by one, its evades +6 to 0, the heat
         * +0xC to 1.0; the car impounded once the strikes reach +2); the other (0xB8A7C6CC) spends an Out of Jail
         * marker instead (type 0x11, 0x576C90), no strike.  The bot pays, unless this strike would impound the car
         * (strikes + 1 >= the most it takes) and a marker is held: then it spends the marker (M4-48).  The marker's
         * button is beside the default one: the bot steps LEFT, then RIGHT, until it is highlighted (a few tries,
         * then it pays). */
        uint32_t cr = career_rec(profile(), rd(profile() + 0xa8u));
        int strikes = cr ? MR8(cr + 3u) : 0, most = cr ? MR8(cr + 2u) : 0;
        B.sheet_seen = 1;
        if (S.jail_since != S.since){       /* the sheet came (again) */
            S.jail_since = S.since; B.jail_moves = 0;
            fprintf(stderr, "[mwbot] f=%ld the infractions sheet: the current car's strikes %d of %d, Out of Jail markers %d\n",
                    f, strikes, most, B.oojail);
        }
        int marker = cr && most > 0 && strikes + 1 >= most && B.oojail > 0 && !B.pay_only;
        if (marker && B.jail_moves < 6){
            if (focus == F_YES_DLG){
                autotest_tap(DIK_RETURN, 6); S.last = f; S.taps++;
                B.jail_spent++; B.jail_moves = 99;   /* once: the sheet's next answer (if it stays) is the default */
                fprintf(stderr, "[mwbot] f=%ld %s @%08x: Enter: an Out of Jail marker spent (strikes %d of %d, markers %d)\n",
                        f, nm, focus, strikes, most, B.oojail);
                return;
            }
            /* Pay_with_Marker is LEFT of Pay_with_Cash (V c4-imp-a) */
            key = B.jail_moves++ & 1 ? DIK_RIGHT : DIK_LEFT; what = "to the Out of Jail marker (the strike would impound the car)";
        }
        else if (focus == F_YES_DLG && B.jail_moves < 6){
            /* the sheet opened on Pay_with_Marker (bin 15's first bust, with the tutorial's Dialog 'When you get busted,
             * you'll need to...': V c4-imp-c, where Enter there spent a marker): the bot keeps its markers for the
             * strike that would impound the car, and goes to Pay_with_Cash */
            key = B.jail_moves++ & 1 ? DIK_LEFT : DIK_RIGHT; what = "to Pay with Cash (the marker kept)";
        }
        else what = "Enter: pay the fines (a strike on the car)";
    }
    else if (!strcasecmp(nm, "SafehouseMarkers.fng")){
        /* the beaten rival's markers ("You can now select 2 markers", the cards face down): Enter turns the highlighted
         * card and takes it, and the highlight stays on that card, so the next is RIGHT, then Enter (bot run of
         * 2026-10-30: Enter again on the taken card did nothing, ten times) */
        /* NFSMW_BOT_MARKERS=X,Y (test-only): the two cards to take, by their objects (hex): the highlight opens on
         * 0xCDA0A66B and RIGHT steps to 0xCDA0A66C, 0xCDA0A66D (V c4-bl14-1), so other cards than the first two can
         * be taken, the way a player picks them (M4-14: each marker kind once) */
        static uint32_t mk_want[2]; static int mk_parsed;
        if (!mk_parsed){ const char *e = getenv("NFSMW_BOT_MARKERS"); mk_parsed = 1; if (e) sscanf(e, "%x,%x", &mk_want[0], &mk_want[1]); }
        uint32_t tgt = S.mk_taken < 2 ? mk_want[S.mk_taken] : 0u;
        if (tgt && focus != tgt && S.mk_moves < 12){
            key = focus < tgt ? DIK_RIGHT : DIK_LEFT; S.mk_moves++; what = "to the marker asked for (NFSMW_BOT_MARKERS)";
        }
        else if (tgt){ what = "Enter: take the marker asked for"; S.mk_taken++; S.mk_moves = 0; S.marker_enter = 1; S.shown = 0; }
        else if (S.marker_enter){ key = DIK_RIGHT; what = "RIGHT to another marker"; S.marker_enter = 0; }
        else { what = "Enter: take the highlighted marker"; S.marker_enter = 1; S.mk_taken++; S.shown = 0; }   /* the card turned: logged */
    }
    else if (B.st == B_SHOP && !strcasecmp(nm, "CustomizeMain.fng")){
        if (!B.sh_started){                 /* in: once per bin (or SHOP_AGAIN more cash), whatever comes of it */
            B.sh_started = 1; B.sh_cat = 0; B.sh_cat_done = 0; B.shop_bin = B.g.bin; B.shop_cash = B.g.cash;
        }
        if (B.sh_cat >= SHOP_CATS || B.sh_paid){ key = DIK_ESCAPE; what = "Escape: leave the shop"; }
        else if (focus != F_ITEM2){ key = focus < F_ITEM2 ? DIK_RIGHT : DIK_LEFT; what = "to Performance"; }
        else what = "Enter on Performance";
    }
    else if (B.st == B_SHOP && !strcasecmp(nm, "CustomizeCategory.fng")){
        if (B.sh_cat_done){ B.sh_cat++; B.sh_cat_done = 0; }
        uint32_t want = F_ITEM1 + (uint32_t)shop_cat();
        if (B.sh_cat >= SHOP_CATS){ key = DIK_ESCAPE; what = "Escape: the packages are done (to the cart if it holds any)"; }
        else if (focus != want){ key = focus < want ? DIK_RIGHT : DIK_LEFT; what = "to the next package"; }
        else { what = "Enter on the package"; B.sh_pk_new = 1; }
    }
    else if (B.st == B_SHOP && !strcasecmp(nm, "CustomizePerformance.fng")){
        char sh[2048]; mw_fe_shows(sh, sizeof sh);
        long cost = shows_num(sh, "'cost"), funds = shows_num(sh, "'funds:"), total = shows_num(sh, "'total:"),
             trade = shows_num(sh, "'Trade_in:"), nlev = shows_nth(sh, "00000018", 0), idx = shows_nth(sh, "00000018", 2);
        const char *cat = shop_cat() >= 0 ? shop_cat_name[shop_cat()] : "?";
        if (B.sh_pk_new){
            B.sh_pk_new = 0; B.sh_target = (int)nlev; B.sh_entered = 0; B.sh_total0 = total;
            B.sh_inst = installed_level(shop_cat());
            fprintf(stderr, "[mwbot] f=%ld shop: %s, %ld levels, level %d on the car, funds %ld, cart %ld, trade-in %ld\n", f,
                    cat, nlev, B.sh_inst, funds, total, trade);
        }
        if (B.sh_entered){              /* the Enter's answer: the cart's total risen, or not (locked) */
            B.sh_entered = 0;
            if (total > B.sh_total0){
                B.sh_spent += total - B.sh_total0;
                fprintf(stderr, "[mwbot] f=%ld shop: %s level %ld/%ld in the cart (%ld; cart %ld)\n", f, cat, idx, nlev,
                        total - B.sh_total0, total);
                B.sh_cat_done = 1;
            } else {
                fprintf(stderr, "[mwbot] f=%ld shop: %s level %ld/%ld (%ld): not taken (locked)\n", f, cat, idx, nlev, cost);
                B.sh_target = (int)idx - 1;
            }
        }
        if (B.sh_cat_done){ key = DIK_ESCAPE; what = "Escape: the package is done"; }
        else if (B.sh_target < 1 || idx < 1 || nlev < 1 || (B.sh_inst >= 0 && B.sh_target <= B.sh_inst)){
            B.sh_cat_done = 1; key = DIK_ESCAPE; what = "Escape: no level of this package bought (none above the car's)";
        }
        else if (idx != B.sh_target){ key = idx < B.sh_target ? DIK_RIGHT : DIK_LEFT; what = "to a level"; }
        else if (cost > 0 && funds >= 0 && cost <= funds - total - SHOP_RESERVE && cost > 2 * trade){
            B.sh_entered = 1; B.sh_total0 = total; what = "Enter: the level into the cart";
        }
        else {                          /* beyond the money left, or not above the part on the car: the next lower */
            fprintf(stderr, "[mwbot] f=%ld shop: %s level %ld/%ld costs %ld, the money left %ld (%ld kept back): the next lower\n",
                    f, cat, idx, nlev, cost, funds - total - SHOP_RESERVE, (long)SHOP_RESERVE);
            B.sh_target--; return;
        }
    }
    else if (B.st == B_SHOP && !strcasecmp(nm, "ShoppingCart.fng")){ what = "Enter: check out"; B.sh_paid = 1; }
    else if (!strcasecmp(nm, "Car_Select.fng")){
        /* The career's cars ('k / N' in the strings of label 0x18, the list in career-record order): from the safehouse
         * menu for pick_car's car, and after a bust (the sheet, then this).  After a bust the car taken is the current
         * one while it can be driven, else choose_car's (M4-48: after the third strike impounded the car, Enter on it
         * opened a Dialog and brought this screen back, for good, runs c13d and c13t).  An Enter that brings the screen
         * back all the same (a Dialog between: that car was not taken) moves the pick one place RIGHT. */
        char sh[1600]; mw_fe_shows(sh, sizeof sh);
        long k = shows_nth(sh, "00000018", 2);
        if (S.cs_since != S.since){         /* the screen came (again) */
            S.cs_since = S.since; S.cs_open = 0;
            if (B.cs_k && !B.cs_dlg) B.cs_k = 0;   /* back without a Dialog (the list drawn again): the same pick */
            B.cs_dlg = 0;
            if (B.cs_k){
                B.cs_retry++;
                fprintf(stderr, "[mwbot] f=%ld Car_Select.fng again after Enter on %d (try %d): that car was not taken\n", f,
                        B.cs_k, B.cs_retry);
            }
            if (!B.want_car || B.car_entered || B.cs_k){
                uint32_t prof = profile(), cur = rd(prof + 0xa8u), h = 0;
                int pos = 0, n = 0, rec = choose_car(cur, &h, &pos, &n), free_now = car_free(prof, cur);
                if (B.cs_k && n > 0) pos = (B.cs_k > 0 ? B.cs_k : B.cs_retry) % n + 1;   /* the next place */
                if (!free_now && !B.cs_k) B.impounds++;
                B.want_car = h ? h : cur; B.want_pos = pos; B.car_entered = 0;
                fprintf(stderr, "[mwbot] f=%ld Car_Select.fng: the current car %#x %s; to %d / %d (handle %#x, career record %d)\n",
                        f, cur, free_now ? "can be driven" : "is impounded", pos, n, h, rec);
            }
            B.cs_k = 0;
        }
        if (B.cs_retry > 8){ set_state(B_DONE, "Car_Select: no car was taken"); return; }
        if (k >= 1 && !S.cs_open){          /* the list opens on the current car: what that place holds is seen */
            uint32_t prof = profile(), cur = rd(prof + 0xa8u);
            int n = career_cars(prof);
            S.cs_open = 1;
            cs_map_check(n);
            if (k < 33) B.cs_map[k] = cur;
            if (B.want_car){
                int p = cs_pos_for(B.want_car, B.want_pos, n);
                if (p != B.want_pos)
                    fprintf(stderr, "[mwbot] f=%ld Car_Select.fng opens on %ld / %d (the current car %#x): car %#x is at %d, not %d\n",
                            f, k, n, cur, B.want_car, p, B.want_pos);
                B.want_pos = p;
            }
        }
        if (k < 1){ if (f - S.since < 150) return; B.car_entered = 1; B.cs_k = -1; what = "Enter: the list's place not read"; }
        else if (k != B.want_pos){ key = k < B.want_pos ? DIK_RIGHT : DIK_LEFT; what = "to the car picked"; }
        else { B.car_entered = 1; B.cs_k = (int)k; B.cs_ek = (int)k; what = "Enter: the car picked"; }
    }
    else if (!strcasecmp(nm, "MainMenu_Sub.fng") && mw_fe_match("MainMenu_Sub.fng:e596c4a3")){
        /* Save is the menu's last item (the 0x532220 set-up adds it last), and the menu grows with the career (Rap
         * Sheet; five items after the second rival), so the bot goes RIGHT until the highlight stops moving, twice in
         * a row (a tap the menu drops as it comes up is not the end), and takes that item.  (Telling it by the labels
         * drawn failed: the menu draws only some items at a time, and the bot went back and forth between two items
         * for 20,000 frames, bot run of 2026-10-30.) */
        if (!save && B.want_car && !B.car_entered){       /* pick_car: Car Select first, the third item */
            if (focus != F_ITEM1 + 2u){ key = focus < F_ITEM1 + 2u ? DIK_RIGHT : DIK_LEFT; what = "to Car Select"; }
            else what = "Enter on Car Select";
        } else if (!save){
            if (B.want_car && B.car_entered){
                uint32_t cur = rd(profile() + 0xa8u);
                int n = career_cars(profile());
                if (cur != B.want_car && B.cs_ek >= 1 && B.cs_ek < 33 && B.cs_tries < 6){
                    /* Enter took another car: that place holds it; Car Select again, at the place now thought */
                    cs_map_check(n);
                    B.cs_map[B.cs_ek] = cur; B.cs_tries++; B.car_entered = 0;
                    B.want_pos = cs_pos_for(B.want_car, B.want_pos, n);
                    fprintf(stderr, "[mwbot] f=%ld the current car: handle %#x, not the car picked (%#x): Car Select's %d / %d "
                            "holds it; again, to %d (try %d)\n", f, cur, B.want_car, B.cs_ek, n, B.want_pos, B.cs_tries);
                } else {
                    fprintf(stderr, "[mwbot] f=%ld the current car: handle %#x%s\n", f, cur,
                            cur == B.want_car ? " (the car picked)" : ", not the car picked: left as it is");
                    B.want_car = 0; B.cs_tries = 0;
                    if (B.st == B_CAR) B.car_swapped = 1;
                }
            }
            if (B.want_car && !B.car_entered){
                if (focus != F_ITEM1 + 2u){ key = focus < F_ITEM1 + 2u ? DIK_RIGHT : DIK_LEFT; what = "to Car Select (again)"; }
                else what = "Enter on Car Select (again)";
            }
            else if (focus != F_ITEM1){ key = DIK_LEFT; what = "to Resume Free Roam"; } else what = "Enter on Resume Free Roam";
        } else {
            S.save_same = focus == S.save_last ? S.save_same + 1 : 0;
            S.save_last = focus;
            if (S.save_same >= 2) what = "Enter on the last item, Save";
            else { key = DIK_RIGHT; what = "to Save, the last item"; }
        }
    }
    else if (!strcasecmp(nm, "Dialog.fng") || !strcasecmp(nm, "InGameDialog.fng")){
        if (focus == F_NO_DLG){ key = DIK_LEFT; what = "LEFT to Yes / OK"; } else what = "Enter on Yes / OK";
    }
    else if (!strcasecmp(nm, "MC_Main.fng")){
        if (mw_fe_match("MC_Main.fng'Save_complete.")){
            autotest_tap(DIK_RETURN, 6); S.last = f;
            fprintf(stderr, "[mwbot] f=%ld %s: Save complete, Enter\n", f, nm);
            if (save && B.as_resume){ B.as_resume = 0; set_state(B_ROAM, "saved; on from the safehouse (autosave)"); }
            else if (save) set_state(B_DONE, "saved");
            return;
        }
        if (focus == F_YES_DLG){ key = DIK_LEFT; what = "LEFT to Yes"; } else what = "Enter on Yes";
    }
    else if (!strcasecmp(nm, "Pause_Main.fng")){
        int jump = save || (B.st == B_CAR && !B.car_swapped);   /* to save, or to change the car (B_CAR) */
        uint32_t want = jump ? F_ITEM2 : F_ITEM1;   /* Jump to Safe House, or Resume Free Roam */
        if (focus != want){ key = focus < want ? DIK_RIGHT : DIK_LEFT; what = jump ? "to Jump to Safe House" : "to Resume"; }
        else what = jump ? "Enter on Jump to Safe House" : "Enter on Resume";
    }
    else if (!strcasecmp(nm, "InGamePhotoMaster.fng") || !strcasecmp(nm, "PostRace_Results.fng")) B.post_seen = 1;
    else if (!strcasecmp(nm, "PostRace_MilestoneRewards.fng") || !strcasecmp(nm, "PostRace_Pursuit.fng") ||
             !strcasecmp(nm, "Loading_Tips.fng")) ;
    else {                              /* not known: a pause, then Enter a few times */
        if (f - S.since < 150) return;
        if (S.taps >= 8){ if (S.taps == 8){ S.taps++; fprintf(stderr, "[mwbot] f=%ld %s: left as it is\n", f, nm); } return; }
        what = "Enter (a screen the bot does not know)";
    }
    autotest_tap(key, 6); S.last = f; S.taps++;
    fprintf(stderr, "[mwbot] f=%ld %s @%08x: %s\n", f, nm, focus, what);
}

int mw_bot_idle(void){ return B.st == B_OFF || B.st == B_DONE; }

/* the commands (career_mw.c queues them for the main loop's anchor) */
void mw_bot_cmd(CPU *c, const char *args){
    char a1[32] = {0}; int n = 0;
    sscanf(args, "%31s %d", a1, &n);
    long f = autotest_frame();
    if (!strcasecmp(a1, "gates")){
        struct gates g; struct race r[MAX_RACES];
        if (!read_gates(c, &g)){ fprintf(stderr, "[mwbot] f=%ld gates: no career\n", f); return; }
        log_gates(&g, "asked");
        log_cars();
        milestones_log(c, g.bin);
        { char b[200]; int o = 0;
          for (int i = 0; i < SHOP_CATS; i++) o += snprintf(b + o, sizeof b - (size_t)o, " %s %d", shop_cat_name[i], installed_level(i));
          fprintf(stderr, "[mwbot]   the current car's performance levels:%s\n", b); }
        log_races(r, read_races(c, g.binobj, r, MAX_RACES, 0));
        fprintf(stderr, "[mwbot]   the rival's races:\n");
        log_races(r, read_races(c, g.binobj, r, MAX_RACES, 1));
        return;
    }
    if (!strcasecmp(a1, "off")){ mw_aid_autopilot(c, 0); set_state(B_OFF, "bot off"); return; }
    memset(&B, 0, sizeof B);
    if (!strcasecmp(a1, "run")){
        char a3[16] = {0}; int k = 0;
        sscanf(args, "%*s %*d %15s %d", a3, &k);
        B.target = n; B.stop_races = !strcasecmp(a3, "races"); B.stop_wins = B.stop_races ? k : 0;
        B.stop_ready = !strcasecmp(a3, "ready");
        { const char *e = getenv("NFSMW_BOT_AUTOSAVE"); B.as_on = e && atoi(e) > 0; }
        B.run_f0 = f;
        if (strcasecmp(a3, "keep")) pick_car();         /* bot run N keep: race the current car */
        mw_aid_autopilot(c, 2);
        set_state(B_ROAM, B.stop_wins ? "bot run, until K races are won" : B.stop_races ? "bot run, until the races are won"
                        : B.stop_ready ? "bot run, until the rival's requirements are met" : "bot run");
        return;
    }
    if (!strcasecmp(a1, "save")){ S.save_same = 0; S.save_last = 0; set_state(B_SAVE, "bot save"); return; }
    if (!strcasecmp(a1, "pursuit")){ B.once = 1; read_gates(c, &B.g); set_state(B_PURSUIT, "bot pursuit"); return; }
    if (!strcasecmp(a1, "car")){        /* a probe of B_CAR: the career's next car taken at the safehouse, then stop */
        uint32_t h = next_car(); int pos = 0, nn = 0; uint32_t hh = 0;
        if (!h){ fprintf(stderr, "[mwbot] f=%ld bot car: no other car that can be driven\n", f); return; }
        choose_car(h, &hh, &pos, &nn);
        B.once = 1; B.want_car = h; B.want_pos = pos; read_gates(c, &B.g);
        fprintf(stderr, "[mwbot] f=%ld bot car: car %#x (Car Select's %d / %d) at the safehouse\n", f, h, pos, nn);
        set_state(B_CAR, "bot car"); return;
    }
    if (!strcasecmp(a1, "bust")){       /* a probe of the impound path (M4-48): the strikes, the markers, Car_Select */
        char a3[16] = {0};
        sscanf(args, "%*s %*d %15s", a3);
        B.once = 1; B.stand = 1; B.bust_n = n > 0 ? n : 1; B.pay_only = !strcasecmp(a3, "pay"); read_gates(c, &B.g);
        fprintf(stderr, "[mwbot] f=%ld bot bust: pursuits with the car standing until %d busts%s\n", f, B.bust_n,
                B.pay_only ? " (the fines paid, no Out of Jail marker)" : "");
        set_state(B_PURSUIT, "bot bust"); return;
    }
    if (!strcasecmp(a1, "chase")){
        B.once = 1; B.chase_s = n > 0 ? n : 1800; read_gates(c, &B.g);
        fprintf(stderr, "[mwbot] f=%ld bot chase: pursuits for %.0f game seconds\n", f, B.chase_s);
        set_state(B_PURSUIT, "bot chase"); return;
    }
    if (!strcasecmp(a1, "race")){
        struct gates g; struct race r[MAX_RACES];
        read_gates(c, &g); B.g = g;
        int k = read_races(c, g.binobj, r, MAX_RACES, 0);
        if (n < 0 || n >= k || !r[n].params){ fprintf(stderr, "[mwbot] f=%ld race %d: no such race (%d)\n", f, n, k); return; }
        B.once = 1; B.race = n;
        mw_aid_autopilot(c, 2);
        fprintf(stderr, "[mwbot] f=%ld bot race %d: to its start\n", f, n);
        go_to(c, &r[n], r, k, 0);
        return;
    }
    fprintf(stderr, "[mwbot] bot gates | run N | race I | pursuit | chase S | bust N [pay] | car | save | off\n");
}
