/* Most Wanted's career bot (TRACKER M4-15; test only, like the race aids of career_mw.c, which it uses).  It plays
 * the career forward the way a player does, through the game's own paths: it reads the Blacklist's gates the way
 * the Blacklist screen reads them, engages the current bin's races at their own start (the engage trigger is at the
 * race's start grid), lets the game's AI drive them (career_mw.c's autopilot, with its wedge reset), answers the
 * screens that come, and earns bounty and milestones in pursuits the game runs.  It never writes progression
 * (L-022): every count it waits for is the game's.
 *
 *   bot gates       log the current bin's gates and its races (below)
 *   bot run N       play until the career's bin is N (or lower), then stop (`bot off` stops it at once)
 *   bot race I      engage race I of the bin's list and race it, then stop (a probe)
 *   bot pursuit     one pursuit in free roam, then stop (a probe)
 *   bot save        save at the safehouse: the pause menu's Jump to Safe House, then the safehouse menu's Save
 *   bot wait [N]    (career_mw.c) wait up to N frames for the bot to stop
 *
 * Its order in a bin (decide): the bin's races not won yet, the one tried least first, until the races the rival asks
 * are won; then pursuits until the milestones and the bounty are there; then the rival's challenge, engaged at its
 * first race's start (InGameRivalChallenge.fng, whose Enter accepts it).  Cops on the car in free roam (a pursuit
 * that began in a race goes on after it) are a pursuit first, whatever it was doing.  In a pursuit the AI drives
 * (pursuit_frame: what was tried besides, and dropped).  A bust adds a strike to the car, and with the career's one
 * car three busts end the career (GameOver.fng): the bot stops there, and on any career menu it does not know.
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
 * safehouse menu, the pause menu and their dialogs as the state wants (below); any other screen that takes input
 * (after a bust: Infractions.fng, Car_Select.fng) is logged and answered with Enter after a pause, a few times, then
 * left (the log says so). */
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
double mw_game_time(void);

#define G_PROFILES       0x0091cf90u     /* [+0x10] the user profile */
#define PROF_BIN         0xb0u
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
#define PURSUIT_STATE    0x218u          /* 0 the chase, 2 the cops' search (the cooldown, +0xFC filling to 1), 3 busted */
#define DIK_ESCAPE 0x01u
#define DIK_RETURN 0x1cu
#define DIK_LEFT   0xcbu
#define MAX_RACES 32

static uint32_t rd(uint32_t a){ return a && mem_is_committed(a) && mem_is_committed(a + 3u) ? MR32(a) : 0u; }
static float rdf(uint32_t a){ uint32_t v = rd(a); float f; memcpy(&f, &v, 4); return f; }
static uint32_t call(CPU *c, uint32_t fn, uint32_t self, int n, const uint32_t *args){ return mw_aid_call(c, fn, self, n, args); }
static uint32_t call1(CPU *c, uint32_t fn, uint32_t self, uint32_t a){ return mw_aid_call(c, fn, self, 1, &a); }

/* ---- the career's state ---- */
struct gates { int bin; uint32_t binobj; int won, won_req, ms, ms_req, bounty, bounty_req; };
static uint32_t profile(void){ return rd(rd(G_PROFILES) + 0x10u); }
static int read_gates(CPU *c, struct gates *g){
    memset(g, 0, sizeof *g);
    uint32_t prof = profile(), db = rd(G_RACEDB);
    if (!prof || !db) return 0;
    g->bin = MR8(prof + PROF_BIN);
    g->binobj = call1(c, FN_DB_BIN, db, (uint32_t)g->bin);
    g->bounty = (int)call(c, FN_BOUNTY, prof + PROF_CARDB, 0, 0);
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
    fprintf(stderr, "[mwbot] f=%ld t=%.1f gates (%s): bin %d races won %d/%d milestones %d/%d bounty %d/%d\n",
            autotest_frame(), mw_game_time(), why, g->bin, g->won, g->won_req, g->ms, g->ms_req, g->bounty, g->bounty_req);
}
static void log_races(const struct race *r, int n){
    for (int i = 0; i < n; i++)
        fprintf(stderr, "[mwbot]   race %d hash %08x key %08x flags %x %s %.2f %.2f %.2f facing %.3f %.3f %.3f%s\n", i,
                r[i].hash, r[i].key, r[i].flags, r[i].trig ? "trigger at" : "start grid at", r[i].pos[0], r[i].pos[1],
                r[i].pos[2], r[i].dir[0], r[i].dir[1], r[i].dir[2], r[i].flags & 2u ? " (completed)" : "");
}

/* ---- the bot ---- */
enum { B_OFF, B_ROAM, B_GOTO, B_ENGAGE, B_RACE, B_PURSUIT, B_SAVE, B_DONE };
static const char *const st_name[] = { "off", "roam", "goto", "engage", "race", "pursuit", "save", "done" };
#define SETTLE_S      1.5               /* game seconds in free roam before the bot decides */
#define ENGAGE_TRIES  5                 /* Enters at a race's start before it is skipped for now */
#define ENGAGE_EVERY  90                /* frames between them */
#define RACE_START_MAX 6000             /* frames from Accept to the race going active */
#define PURSUIT_NATURAL_S 60.0          /* game seconds of free driving for a pursuit to start by itself */
#define SKIP_MAX      3
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
    uint32_t focus0, pursuit;
    int bounty0, forced;
    int after;                          /* B_PURSUIT: the state to go back to (0: B_ROAM, or the probe's end) */
    double next_log;
    float plen;                         /* the pursuit's length as it last showed it (s) */
    struct gates g;
} B;
static void set_state(int st, const char *why){
    fprintf(stderr, "[mwbot] f=%ld t=%.1f %s -> %s%s%s\n", autotest_frame(), mw_game_time(), st_name[B.st], st_name[st],
            why ? ": " : "", why ? why : "");
    B.st = st; B.t0 = mw_game_time(); B.f0 = autotest_frame(); B.tries = 0; B.free_since = 0; B.go_set = 0;
    B.fin_f = 0; B.post_seen = 0;
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
static void go_to(CPU *c, const struct race *r){
    mw_aid_autopilot(c, AP_STAND);             /* the car stands still at the start */
    memcpy(B.tpos, r->pos, sizeof B.tpos); memcpy(B.tdir, r->dir, sizeof B.tdir);
    mw_aid_teleport(c, r->pos, r->dir);
    B.warps = 1;
    set_state(B_GOTO, 0);
}
static void decide(CPU *c){
    struct gates g; struct race r[MAX_RACES];
    if (!read_gates(c, &g)){ set_state(B_DONE, "no career"); return; }
    if (memcmp(&g, &B.g, sizeof g)) log_gates(&g, "now");
    B.g = g;
    if (B.target && g.bin <= B.target){
        fprintf(stderr, "[mwbot] f=%ld t=%.1f reached bin %d\n", autotest_frame(), mw_game_time(), g.bin);
        set_state(B_DONE, "the target bin"); return;
    }
    int n = read_races(c, g.binobj, r, MAX_RACES, 0), pick = -1;
    if (g.won < g.won_req)              /* the race not won yet that was tried the least (a lost race waits its turn) */
        for (int i = 0; i < n; i++)
            if (!(r[i].flags & 2u) && r[i].params && B.skips[i] < SKIP_MAX && (pick < 0 || B.tried[i] < B.tried[pick])) pick = i;
    if (pick >= 0){
        B.race = pick; B.tried[pick]++;
        fprintf(stderr, "[mwbot] f=%ld t=%.1f race %d (hash %08x, try %d): to its start %.2f %.2f %.2f\n", autotest_frame(),
                mw_game_time(), pick, r[pick].hash, B.tried[pick], r[pick].pos[0], r[pick].pos[1], r[pick].pos[2]);
        go_to(c, &r[pick]); return;
    }
    if (g.won < g.won_req){ log_races(r, n); set_state(B_DONE, "races to win, none left to engage"); return; }
    if (g.ms < g.ms_req || g.bounty < g.bounty_req){ set_state(B_PURSUIT, "milestones or bounty to earn"); return; }
    /* the rival's challenge: its first race is engaged at its start like the others */
    struct race b[8];
    int nb = read_races(c, g.binobj, b, 8, 1);
    if (!B.boss_logged){ B.boss_logged = 1; log_races(r, n); fprintf(stderr, "[mwbot]   the rival's races:\n"); log_races(b, nb); }
    int done = 0;
    for (int i = 0; i < nb; i++) done += (b[i].flags & 2u) != 0;
    if (done && done < nb && autotest_frame() - B.f0 < RIVAL_NEXT_WAIT) return;   /* its next race may come by itself */
    if (nb > 0 && b[0].params && B.boss_tries < SKIP_MAX){
        B.boss_tries++; B.race = -1;
        fprintf(stderr, "[mwbot] f=%ld t=%.1f the rival's challenge (hash %08x, try %d, races won %d of %d): to its "
                "start %.2f %.2f %.2f\n", autotest_frame(), mw_game_time(), b[0].hash, B.boss_tries, done, nb, b[0].pos[0],
                b[0].pos[1], b[0].pos[2]);
        go_to(c, &b[0]); return;
    }
    set_state(B_DONE, "the rival's requirements are met; the challenge did not engage");
}

static void pursuit_frame(CPU *c){
    uint32_t perp = 0, po = mw_aid_pursuit(c, &perp);
    double t = mw_game_time();
    long f = autotest_frame();
    if (t < B.t0) B.t0 = t;             /* the world was loaded again (a bust): its clock starts again */
    if (!B.raced){                       /* no pursuit yet: the AI drives about until the cops start one */
        if (B.tries == 0 && game_flow() == g_game.flow_race && !mw_fe_input_top(0)){
            mw_aid_autopilot(c, 1); B.tries = 1; B.bounty0 = B.g.bounty;
        }
        if (po){
            B.raced = 1; B.pursuit = po; B.next_log = t;
            fprintf(stderr, "[mwbot] f=%ld t=%.1f pursuit %08x began%s: heat %.2f range %.2f-%.2f cops %u\n", f, t, po,
                    B.forced ? " (forced)" : " (by itself)", rdf(perp + PERP_HEAT), rdf(po + 0x9cu), rdf(po + 0xa0u),
                    rd(po + PURSUIT_COPS));
        } else if (B.tries == 1 && t - B.t0 >= PURSUIT_NATURAL_S && free_roam(c)){
            call1(c, FN_FORCE_PURSUIT, 0, 1u);
            B.forced = 1; B.tries = 2;
            fprintf(stderr, "[mwbot] f=%ld t=%.1f no pursuit after %.0f s of driving: Game.ForcePursuitStart(1)\n", f, t,
                    t - B.t0);
        }
        return;
    }
    if (po){
        B.plen = rdf(po + PURSUIT_TIME);
        /* The AI drives the whole pursuit.  What was tried to make chases longer or to save a boxed-in car from the bust,
         * and dropped (bot runs of 2026-10-30): letting the car stand when the cops lost it (state +0x218 == 2, the
         * cops' search) did not bring them back, the cooldown ran out, and cops that did come busted the standing car;
         * the game's RESET when the bust timer (+0xDC) passed 1.5 s at walking speed put the car on the road beside
         * the same cops, the timer ran on (8.6 four seconds later) and the next reset found no place; every pursuit of
         * that run ended busted, three busts impounded the only car, and the career ended (GameOver.fng).  With the AI
         * alone two pursuits of about ten ended busted. */
        if (t >= B.next_log){
            B.next_log = t + 15.0;
            float kmh = 0, p[3] = { 0 }; mw_aid_where(c, p, &kmh);
            fprintf(stderr, "[mwbot] f=%ld t=%.1f pursuit %.0f s: state %u heat %.2f cops %u bust %.1f perp bounty %u at %.0f "
                    "%.0f %.0f %.0f km/h\n", f, t, rdf(po + PURSUIT_TIME), rd(po + PURSUIT_STATE), rdf(perp + PERP_HEAT),
                    rd(po + PURSUIT_COPS),
                    rdf(po + PURSUIT_BUST_T), rd(perp + 0x20u), p[0], p[1], p[2], kmh);
        }
        return;
    }
    if (!settled(c)) return;            /* the pursuit's end: its screens first */
    struct gates g; read_gates(c, &g);
    fprintf(stderr, "[mwbot] f=%ld t=%.1f pursuit over (%.0f s long, %.0f s of game time in all): bounty %d -> %d, "
            "milestones %d -> %d\n", f, t, B.plen, t - B.t0, B.bounty0, g.bounty, B.g.ms, g.ms);
    mw_aid_autopilot(c, AP_STAND);
    B.raced = 0; B.pursuit = 0; B.forced = 0;
    if (B.once && !B.after){ set_state(B_DONE, "the probe's pursuit is over"); return; }
    int back = B.after ? B.after : B_ROAM;
    B.after = 0;
    set_state(back, "pursuit over");
}

void mw_bot_frame(CPU *c){
    if (B.st == B_OFF || B.st == B_DONE) return;
    long f = autotest_frame();
    switch (B.st){
    case B_ROAM:
        if (pursuit_on(c, B_ROAM)) break;
        if (game_flow() == g_game.flow_race && racing(c)){ set_state(B_RACE, "a race began (the rival's next)"); break; }
        if (settled(c)) decide(c);
        break;
    case B_GOTO: {                      /* at the start: Enter until the engage dialog comes */
        if (pursuit_on(c, B_ROAM)) break;
        if (mw_fe_input_top(0)) break;  /* a screen (the dialog itself: B_ENGAGE takes it in mw_bot_present) */
        if (!settled(c) || f - B.last_tap < ENGAGE_EVERY) break;
        float p[3], kmh = 0;            /* not there (the warp undone, or a menu's reload put it back): again */
        if (mw_aid_where(c, p, &kmh) && hypotf(p[0] - B.tpos[0], p[2] - B.tpos[2]) > GOTO_NEAR_M){
            if (B.warps >= 3){ if (B.race >= 0) B.skips[B.race]++; set_state(B_ROAM, "the car does not stay at the start"); break; }
            fprintf(stderr, "[mwbot] f=%ld the car is at %.1f %.1f %.1f, %.0f m from the start: warped again\n", f, p[0], p[1],
                    p[2], hypotf(p[0] - B.tpos[0], p[2] - B.tpos[2]));
            mw_aid_teleport(c, B.tpos, B.tdir); B.warps++; B.free_since = 0;
            break;
        }
        if (B.tries >= ENGAGE_TRIES){
            if (B.race >= 0) B.skips[B.race]++;
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
        if (!B.fin_f || !settled(c)) break;
        if (!B.post_seen && f - B.fin_f < POST_WAIT) break;
        {
            struct gates g; struct race r[MAX_RACES];
            read_gates(c, &g);
            int n = read_races(c, g.binobj, r, MAX_RACES, 0);
            fprintf(stderr, "[mwbot] f=%ld t=%.1f race %d over: %s; races won %d -> %d, bounty %d -> %d\n", f,
                    mw_game_time(), B.race, B.race < 0 ? "(the rival's)" : B.race < n && (r[B.race].flags & 2u) ? "completed" : "not completed", B.g.won,
                    g.won, B.g.bounty, g.bounty);
            mw_aid_autopilot(c, AP_STAND);
            if (B.once){ set_state(B_DONE, "the probe's race is over"); break; }
            set_state(B_ROAM, "race over");
        }
        break;
    case B_PURSUIT: pursuit_frame(c); break;
    case B_SAVE:                        /* from free roam: the pause menu (the screens do the rest, mw_bot_present) */
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
static struct { char name[64]; uint32_t focus; long since, last; int taps, marker_enter; } S;
void mw_bot_present(void){
    if (B.st == B_OFF || B.st == B_DONE) return;
    uint32_t focus = 0;
    const char *nm = mw_fe_input_top(&focus);
    long f = autotest_frame();
    if (!nm){ S.name[0] = 0; return; }
    if (strcmp(nm, S.name) || focus != S.focus){
        if (strcmp(nm, S.name)){ S.taps = 0; S.since = f; }
        fprintf(stderr, "[mwbot] f=%ld screen %s @%08x (%s)\n", f, nm, focus, st_name[B.st]);
        snprintf(S.name, sizeof S.name, "%s", nm); S.focus = focus;
    }
    if (f - S.last < 45 || f - S.since < 30) return;
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
    else if (!strcasecmp(nm, "SafehouseMarkers.fng")){
        /* the beaten rival's markers ("You can now select 2 markers", the cards face down): Enter turns the highlighted
         * card and takes it, and the highlight stays on that card, so the next is RIGHT, then Enter (bot run of
         * 2026-10-30: Enter again on the taken card did nothing, ten times) */
        if (S.marker_enter){ key = DIK_RIGHT; what = "RIGHT to another marker"; S.marker_enter = 0; }
        else { what = "Enter: take the highlighted marker"; S.marker_enter = 1; }
    }
    else if (!strcasecmp(nm, "MainMenu_Sub.fng") && mw_fe_match("MainMenu_Sub.fng:e596c4a3")){
        uint32_t want = !save ? F_ITEM1 : mw_fe_match("MainMenu_Sub.fng:ac22f27e") ? F_ITEM1 + 4u : F_ITEM1 + 3u;
        if (focus != want){ key = focus < want ? DIK_RIGHT : DIK_LEFT; what = save ? "to Save" : "to Resume Free Roam"; }
        else what = save ? "Enter on Save" : "Enter on Resume Free Roam";
    }
    else if (!strcasecmp(nm, "Dialog.fng") || !strcasecmp(nm, "InGameDialog.fng")){
        if (focus == F_NO_DLG){ key = DIK_LEFT; what = "LEFT to Yes / OK"; } else what = "Enter on Yes / OK";
    }
    else if (!strcasecmp(nm, "MC_Main.fng")){
        if (mw_fe_match("MC_Main.fng'Save_complete.")){
            autotest_tap(DIK_RETURN, 6); S.last = f;
            fprintf(stderr, "[mwbot] f=%ld %s: Save complete, Enter\n", f, nm);
            if (save) set_state(B_DONE, "saved");
            return;
        }
        if (focus == F_YES_DLG){ key = DIK_LEFT; what = "LEFT to Yes"; } else what = "Enter on Yes";
    }
    else if (!strcasecmp(nm, "Pause_Main.fng")){
        uint32_t want = save ? F_ITEM2 : F_ITEM1;   /* Jump to Safe House, or Resume Free Roam */
        if (focus != want){ key = focus < want ? DIK_RIGHT : DIK_LEFT; what = save ? "to Jump to Safe House" : "to Resume"; }
        else what = save ? "Enter on Jump to Safe House" : "Enter on Resume";
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
        log_races(r, read_races(c, g.binobj, r, MAX_RACES, 0));
        fprintf(stderr, "[mwbot]   the rival's races:\n");
        log_races(r, read_races(c, g.binobj, r, MAX_RACES, 1));
        return;
    }
    if (!strcasecmp(a1, "off")){ mw_aid_autopilot(c, 0); set_state(B_OFF, "bot off"); return; }
    memset(&B, 0, sizeof B);
    if (!strcasecmp(a1, "run")){ B.target = n; mw_aid_autopilot(c, 2); set_state(B_ROAM, "bot run"); return; }
    if (!strcasecmp(a1, "save")){ set_state(B_SAVE, "bot save"); return; }
    if (!strcasecmp(a1, "pursuit")){ B.once = 1; read_gates(c, &B.g); set_state(B_PURSUIT, "bot pursuit"); return; }
    if (!strcasecmp(a1, "race")){
        struct gates g; struct race r[MAX_RACES];
        read_gates(c, &g); B.g = g;
        int k = read_races(c, g.binobj, r, MAX_RACES, 0);
        if (n < 0 || n >= k || !r[n].params){ fprintf(stderr, "[mwbot] f=%ld race %d: no such race (%d)\n", f, n, k); return; }
        B.once = 1; B.race = n;
        mw_aid_autopilot(c, 2);
        fprintf(stderr, "[mwbot] f=%ld bot race %d: to its start\n", f, n);
        go_to(c, &r[n]);
        return;
    }
    fprintf(stderr, "[mwbot] bot gates | run N | race I | pursuit | off\n");
}
