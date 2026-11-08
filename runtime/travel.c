/* travel.c -- Tracks: career free roam on the race tracks' own maps (the page menu's Tracks, F9).
 *
 * The career's drag strips, drift arenas, Street X tracks and Speedways are maps of their own beside the city's
 * (GLOBALB's track table, chunk 0x34201: region L4RB the drags 42xx, L4RC the drifts 43xx, L4RF Street X 46xx, L4RG
 * the URL's 47xx), each with a track numbered xx00 in the table and its free-roam data (TRACKS/ROUTESL4Rx/
 * PathsFreeRoam.bin), as the city's 4000 has; the game only ever races on them.  A trip is free roam loaded again,
 * as co-op's reload (coop.c): the race status's event 10 (SwitchTrack) runs free roam's setup (0x540780; coop.c's
 * override calls travel_setup after it), which writes track 4000, travel_setup the trip's instead, and the flow
 * step (0x581820) finds another region and loads it whole: a Loading screen, 2-4 s with the files at hand.  Back to
 * the city the same way, at the place the car left.  Between two spots of one map (the Airport Circuit and Bayview
 * Speedway share one): the hot-position jump, no load (free roam loaded again on its own track takes the flow step's
 * short way, which leaves the car where it is, and the race status then took no SwitchTrack again).  Free roam's own
 * track number tells the maps apart: 4000 the city, xx00 a track's map.
 *
 * The car starts on a race's start line (SPOTS: the line of the layout's Paths file, chunk 0x34148's first node,
 * heading to its second; the height of the map's road point there, 1 m up: "travel near"), or a little up its route
 * (Routes<n>F.bin: x, y, the distance) where the start's surroundings press the camera onto the car's roof (a tunnel,
 * a net overhead).
 *
 * The start's place is the career's kept spot itself (Career +0x8750, G_START_POS): where free roam's setup puts the
 * car, where leaving free roam keeps the car's place (the pause menu's Quit, 0x513B30), what the profile saves.  On a
 * track's map (T.spot, from the frame it is there):
 *  - the kept spot is the city's place the car left: every frame (but while a trip is on its way: its start is there),
 *    for the pause menu's Save (savegame.c asks travel_city), and before free roam's setup when the game itself loads
 *    it again (travel_presetup).  A save made on a track's map loads in the city, in the stock game too.  Once the
 *    front end is up (the pause menu's Quit) the career is left so: another profile may load next;
 *  - the World Map (M) stays shut: it is the city's (HOOK_SITE 0x602EEF: no map, as in Quick Race's Free Run);
 *  - free-roam multiplayer meets the players at the same spot only (roam_game.c room_at: the room's hash with the
 *    spot's key mixed in), whose count the page's Tracks asks the server for; outruns between them go as in the city
 *    (tests/scripts/travel_outrun_a.txt);
 *  - no traffic (the maps' roads are not free roam's: 0x5DCE50 finds no 4000 among a road's tracks), no career walls
 *    (walls.c looks at the city only), no events or shops: no markers there.  The Respawn key works (the map's roads);
 *  - no tuners: the outrun challengers' manager is never told an outrun is due (sub_0041ace0).
 *
 * A trip is asked by the page (IEV_TRAVEL: travel_ask, from the input thread) or a test script (travel N), and goes
 * only while the car drives alone in career free roam: a car of its own (not a new career's prologue), no outrun,
 * race lobby or race's trip (roam_game_alone), no player 2 (co-op's reload would put the car at the place it is on
 * the city's map; a join waits for the city, coop.c), no timed career task (Career +0xC08: a cover shoot's or a
 * delivery's clock), not on an event's or a shop's marker (Engage would take it).  The page reads the status table's
 * "travel" (TRAVEL_HAS + why * 256 + spot): where the car is and why a trip cannot go now.  Logs: "[travel]".  Script
 * command (tests): travel [N|city|where|near X Y|friend NAME [SPOT]]; tests/scripts/travel_*.txt. */
#include "cpu.h"
#include "guest.h"
#include "game.h"
#include "featurelist.h"
#include "splitscreen_mem.h"
#include "widescreen.h"           /* HOOK_SITE */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>

long autotest_frame(void);
const char *autotest_top_fng(void);
int mprace_car_ok(void);          /* mprace.c: the career has a car of its own */
int roam_game_alone(void);        /* roam_game.c: no outrun, race lobby or race's trip of ours under way */
int coop_wanted(void);            /* coop.c: player 2 is in, or comes back */
int roam_game_friend_at(uint32_t name, float p[3], uint16_t *h);   /* roam_game.c: a linked player's car, by its name's hash */
int walls_open_near(const float at[3], const float near[3], float tx, float ty, float out[3], uint16_t *h);   /* walls.c */
void roam_game_jumped(void);      /* roam_game.c: friends' cars are ghosts until they are clear of ours */
uint32_t rc_room_hash(const char *s);   /* roam_core.c: a name's hash, as the page's roomHash (tests' "travel friend") */

/* status_web.c in the browser; the Mac has no page */
__attribute__((weak)) int host_status_slot(const char *name){ (void)name; return -1; }
__attribute__((weak)) void host_status_set_n(const int *slot, const double *value, int n){ (void)slot; (void)value; (void)n; }

#define G_PLAYER1       SPLIT_PLAYERS_INDEX   /* Player*: +4 its car */
#define PL_MARKER       0x9acu        /* Player: the event's or shop's marker Engage would enter, 0 none */
#define C_TIMED         0xc08u        /* Career: the other event being run (a cover shoot, a delivery: a clock), 0 none */
#define CITY            4000u         /* free roam's track: the city */
#define GOING_FRAMES    600           /* frames in the world with no load for a trip: it never started, given up */
#define TRAVEL_HAS      65536.0       /* the status's "travel": this module takes trips */
#define FRIEND_NEAR_M   60.f          /* a friend this near once the car is on its map: there, no second load */
#define FRIEND_BACK_M   12.f          /* the car goes this far behind the friend (on the nearest road) */
#define FRIEND_WAIT     900           /* frames in the world on the friend's map before it is "not found" (the hub's links) */
#define ROAD_NEAR_M     100.f         /* on a race track's map: a road point this near the place, or no trip there */

/* where a trip goes: the track's map (its xx00), the room's key (a layout's number, roam_game.c), the start line */
static const struct { uint16_t track, key; const char *name; float x, y, z; uint16_t heading; } SPOTS[] = {
    { 4700, 4711, "the Airport Circuit (URL)",     2454.3f,   -272.7f, -10.0f, 0xbf04 },   /* 4712/4713/4715's start line */
    { 4700, 4701, "Bayview Speedway (URL)",       -1987.4f,  -2040.1f,  35.6f, 0x6b33 },   /* 4701/4703-4705's */
    { 4300, 4311, "the Stadium (drift)",          -1174.5f,   -617.1f,   7.6f, 0x81a7 },   /* 4311's */
    { 4300, 4301, "the Parkade (drift)",            877.6f,   -425.4f,  23.1f, 0x57c0 },   /* 4301/4305/4306's */
    { 4200, 4201, "Bayview Bridge (drag)",          994.6f,     36.4f,   3.2f, 0x6630 },   /* 305 m up 4201's route (its start: a tunnel) */
    { 4200, 4211, "Central Station (drag)",        1440.5f,  -1388.1f,   1.5f, 0x9010 },   /* 41 m up 4211's */
    { 4200, 4221, "South Runway (drag)",           2454.6f,  -1498.5f, -10.0f, 0x4000 },   /* 142 m up 4221's (a net over its start) */
    { 4600, 4601, "Industrial Park (Street X)",    -356.3f,   -244.4f,   1.1f, 0xfeb3 },   /* 4601/4602's */
    { 4600, 4605, "the Parkade (Street X)",       -1556.1f,  -2006.7f,  33.2f, 0xa1f4 },   /* 4605/4606/4608's */
};
#define NSPOTS ((int)(sizeof SPOTS / sizeof SPOTS[0]))

/* why a trip cannot go now (the page's text for each: shell.ts TRAVEL_WHY) */
enum { WHY_OK, WHY_NOT_HERE, WHY_NO_CAR, WHY_BUSY, WHY_TIMED, WHY_MARKER, WHY_GOING, WHY_COOP };
static const char *const WHY[] = { "it can go", "not driving in career free roam", "no car of its own yet (the prologue)",
                                   "an outrun, a race lobby or a race's trip", "a timed career task", "on a marker",
                                   "a trip on its way", "player 2 is in" };

static struct {
    int spot;                     /* the spot the car is at: 0 the city, k SPOTS[k - 1] (on a track's map: found from
                                   * its track number and the car's place, spot_on) */
    int going;                    /* a trip on its way: its spot + 1 (1 the city), 0 none */
    int loaded;                   /* ...free roam's setup ran for it (travel_setup): the world after it is the trip's */
    int full;                     /* ...to the map loaded now: coop.c's flow step loads it in full (travel_full) */
    long going_at;                /* ...its last frame out of the world (or its ask): GOING_FRAMES in the world, given up */
    int place; float pp[3]; uint16_t ph;   /* ...to this place on the map (a friend's), not the spot's start */
    float city[3]; uint16_t city_h; int city_ok;   /* the city's place the car left */
    int slot; double shown;
} T = { .slot = -2, .shown = -1 };
static int g_ask;                 /* asked (any thread): spot + 1, ASK_FRIEND a friend (g_ask_name, g_ask_spot), 0 none */
static uint32_t g_ask_name; static int g_ask_spot;
#define ASK_FRIEND      (NSPOTS + 2)
/* a trip to a friend (the page menu's Friends, Join): its name's hash (rc_room_hash), the spot it is at (the page's
 * tracks.ts friendSpot: 0 the city), when asked or on its map; the car goes to that map first, then, once the friend
 * is seen there, behind it (a load of that map with the place: no jump the server's cheat watch would take for a
 * teleport onto a player).  In the city only a place on this career's side of its walls (walls.c walls_open_near) */
static struct { uint32_t name; int spot; long since; int went; } F;
/* the last trip's word to the page (the status's "travel_said": count * 256 + why * 16 + what): SAID_* */
enum { SAID_THERE = 1, SAID_LOCKED, SAID_NOT_FOUND, SAID_NOT_NOW };
static struct { int n, what, why, slot; double shown; } S = { .slot = -2, .shown = -1 };
static int g_test_noevent;        /* (test: NFSU2_TRAVEL_TEST_NOEVENT with a script) a trip asks for no load: given up */

static uint32_t player_car(void){ uint32_t p = MR32(G_PLAYER1); return p ? MR32(p + 4u) : 0; }
static int spot_at(int k){ return k >= 1 && k <= NSPOTS; }
/* a track's map: free roam's own track there */
static int track_map(uint32_t track){ for (int i = 0; i < NSPOTS; i++) if (SPOTS[i].track == track) return 1; return 0; }
/* a career's free roam on a track's map now (its world, or its menus over it): what the kept spot, Save and the World
 * Map look at, whatever T says */
static int on_track(void){ return MR32(G_GAMEMODE) == 1u && MR32(G_FLOW) != 3u && track_map(MR32(G_RACEPARAMS)); }
/* the spot of the loaded track's map the car is nearest to */
static int spot_on(uint32_t track, uint32_t car){
    int best = 0; float bd = 1e30f;
    for (int i = 0; i < NSPOTS; i++){
        if (SPOTS[i].track != track) continue;
        float dx = MRF32(car + 0x60u) - SPOTS[i].x, dy = MRF32(car + 0x64u) - SPOTS[i].y, d = dx * dx + dy * dy;
        if (d < bd){ bd = d; best = i + 1; }
    }
    return best;
}

/* the page's Tracks (input_web.c) or a script: a trip to spot k (0 the city) */
void travel_ask(int k){ if (k >= 0 && k <= NSPOTS) __atomic_store_n(&g_ask, k + 1, __ATOMIC_RELEASE); }
/* the page's Friends (input_web.c): a trip to the friend whose name has this hash, at spot k (0 the city) */
void travel_ask_friend(uint32_t name, int k){
    __atomic_store_n(&g_ask_name, name, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ask_spot, k >= 0 && k <= NSPOTS ? k : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ask, ASK_FRIEND, __ATOMIC_RELEASE);
}
/* roam_game.c: the room's key of the spot the car is at, 0 the city */
uint32_t travel_room(void){ return spot_at(T.spot) ? SPOTS[T.spot - 1].key : 0u; }

/* why a trip to spot k (0 the city) cannot go now, WHY_OK it can */
static int why_not(int k){
    if (T.going) return WHY_GOING;
    uint32_t race = MR32(G_RACE), pl = MR32(G_PLAYER1), car = player_car(), track = MR32(G_RACEPARAMS);
    if (MR32(G_FLOW) != 6u || MR32(G_GAMEMODE) != 1u || MR32(G_RACETYPE) || MR32(G_CAREER_RACE) || !pl || !car ||
        !race || MR32(race) != 4u || (track != CITY && !track_map(track))) return WHY_NOT_HERE;
    const char *top = autotest_top_fng();
    if (!top || strcasecmp(top, "HUD_SingleRace.fng")) return WHY_NOT_HERE;   /* a menu or the map over the world */
    if (MR32(race + 0x24u) != 1u || !roam_game_alone()) return WHY_BUSY;      /* (two racers: an outrun) */
    if (!mprace_car_ok()) return WHY_NO_CAR;
    if (coop_wanted() && k) return WHY_COOP;          /* (back to the city goes: player 2 comes in there, coop.c) */
    if (MR32(G_CAREER + C_TIMED)) return WHY_TIMED;
    if (MR32(pl + PL_MARKER)) return WHY_MARKER;
    return WHY_OK;
}

/* the trip to spot k (0 the city): free roam loaded again, its start there (written by travel_presetup, just before
 * free roam's setup reads it: a trip that never starts changes nothing) */
static void go(CPU *c, int k, long f){
    const char *to = k ? SPOTS[k - 1].name : "the city";
    int why = why_not(k);
    if (why){ fprintf(stderr, "[travel] f=%ld a trip to %s asked: not now, %s\n", f, to, WHY[why]); return; }
    if (k == T.spot){ fprintf(stderr, "[travel] f=%ld a trip to %s asked: there already\n", f, to); return; }
    uint32_t car = player_car(), track = MR32(G_RACEPARAMS);
    if (track == CITY){                               /* leaving the city: where to come back */
        for (uint32_t i = 0; i < 3u; i++) T.city[i] = MRF32(car + 0x60u + 4u * i);
        T.city_h = MR16(car + 0x40cu); T.city_ok = 1;
    }
    if (!T.city_ok){ fprintf(stderr, "[travel] f=%ld a trip to %s asked: no city place kept\n", f, to); return; }
    T.going = k + 1; T.going_at = f; T.loaded = 0; T.place = 0;
    T.full = k && SPOTS[k - 1].track == track;        /* (the map loaded now: coop.c's flow step loads it in full) */
    uint32_t a[2] = { 10u, 0u };
    if (!g_test_noevent) gcall_main(c, FN_POSTEVENT, MR32(G_RACESTATUS), 2, a);
    fprintf(stderr, "[travel] f=%ld to %s (track %u)%s: free roam loads there\n", f, to, k ? SPOTS[k - 1].track : CITY,
            T.full ? ", the map loaded again" : "");
}

/* the career's kept spot: the city's place */
static void keep_city(void){
    for (uint32_t i = 0; i < 3u; i++) MRF32(G_CAREER + C_SPOT + 4u * i) = T.city[i];
    MR16(G_CAREER + C_SPOT_HEADING) = T.city_h;
}
/* savegame.c (the pause menu's Save): on a race track's map, the city's place the car left (what a save keeps), 1;
 * else 0 */
int travel_city(float p[3], uint16_t *h){
    if (!T.city_ok || !on_track()) return 0;
    memcpy(p, T.city, sizeof T.city); *h = T.city_h;
    return 1;
}
/* coop.c's free-roam setup (0x540780), before the game's, which reads the place: a trip's start, or for free roam
 * loaded again by the game's own way while on a track's map (0x513B30 keeping the car's place first) the city's */
void travel_presetup(void){
    long f = autotest_frame();
    if (T.going){
        int k = T.going - 1;
        float p[3]; uint16_t h;
        if (T.place){ memcpy(p, T.pp, sizeof p); h = T.ph; }
        else if (k){ p[0] = SPOTS[k - 1].x; p[1] = SPOTS[k - 1].y; p[2] = SPOTS[k - 1].z; h = SPOTS[k - 1].heading; }
        else { memcpy(p, T.city, sizeof p); p[2] += 0.5f; h = T.city_h; }
        for (uint32_t i = 0; i < 3u; i++) MRF32(G_START_POS + 4u * i) = p[i];
        MR16(G_START_HEAD) = h; MR8(G_START_KEEP) = 1;
        fprintf(stderr, "[travel] f=%ld the start at %.1f %.1f %.1f heading 0x%04x\n", f, (double)p[0], (double)p[1], (double)p[2], h);
        return;
    }
    if (!T.city_ok || !on_track()) return;
    keep_city();
    MR8(G_START_KEEP) = 1;
    fprintf(stderr, "[travel] f=%ld free roam loads again from a track's map by the game's own way: the city's place\n", f);
}
/* coop.c's free-roam setup (0x540780), after the game's (it wrote track 4000): the trip's track */
void travel_setup(void){
    if (!T.going) return;
    T.loaded = 1;
    if (T.going >= 2) MR32(G_RACEPARAMS) = SPOTS[T.going - 2].track;
}
/* coop.c's flow step (0x581820), before the game's: a trip to the map loaded now loads it in full, as co-op's reload
 * (the step's short way for the same region leaves the car where it is, and the race status took no SwitchTrack after
 * it: 2026-11-08), 1 once */
int travel_full(void){
    if (!T.going || !T.full) return 0;
    T.full = 0;
    return 1;
}

/* a trip to place p, heading h, on the map loaded now (spot T.spot's, or the city's): free roam loaded again there */
static void go_place(CPU *c, const float p[3], uint16_t h, long f){
    int k = T.spot;
    T.going = k + 1; T.going_at = f; T.loaded = 0; T.full = 1;
    T.place = 1; memcpy(T.pp, p, sizeof T.pp); T.pp[2] += 1.f; T.ph = h;
    uint32_t a[2] = { 10u, 0u };
    if (!g_test_noevent) gcall_main(c, FN_POSTEVENT, MR32(G_RACESTATUS), 2, a);
    fprintf(stderr, "[travel] f=%ld to %.1f %.1f %.1f on %s: free roam loads there\n", f, (double)p[0], (double)p[1], (double)p[2],
            k ? SPOTS[k - 1].name : "the city");
}
/* on a race track's map: the road point nearest `at` (its height within 6 m of at's, if one is), 1; 0 none within
 * ROAD_NEAR_M (the road table as near_roads reads it) */
static int road_near(const float at[3], float out[3]){
    float bd = ROAD_NEAR_M * ROAD_NEAR_M; int found = 0, level = 0;
    for (uint32_t i = 0; i < 2048u; i++){
        uint32_t r = guest_rd32(0x0088a334u + 4u * i);
        if (!guest_ok(r, 0x8cu) || (int16_t)MR16(r + 0xau) != (int)i) continue;
        int n = (int16_t)MR16(r + 0x34u);
        for (int j = 0; j < n && j < 4096 && guest_ok(r + 0x8cu + 0x38u * (uint32_t)j, 0x38u); j++){
            uint32_t nd = r + 0x8cu + 0x38u * (uint32_t)j;
            float dx = MRF32(nd) - at[0], dy = MRF32(nd + 4u) - at[1], d = dx * dx + dy * dy, z = (float)(int16_t)MR16(nd + 0x12u) / 8.f;
            int lv = fabsf(z - at[2]) < 6.f;
            if (lv < level || (lv == level && d >= bd)) continue;
            bd = d; level = lv; found = 1; out[0] = MRF32(nd); out[1] = MRF32(nd + 4u); out[2] = z;
        }
    }
    return found;
}
/* the trip's word to the page */
static void said(int what, int why, long f){
    static const char *const W[] = { "", "there", "a part of the city this career has not opened", "not found", "not now" };
    S.n++; S.what = what; S.why = why;
    fprintf(stderr, "[travel] f=%ld to a friend: %s%s%s\n", f, W[what], why ? ", " : "", why ? WHY[why] : "");
    double v = (double)S.n * 256 + why * 16 + what;
    if (S.slot == -2) S.slot = host_status_slot("travel_said");
    if (S.slot >= 0) host_status_set_n(&S.slot, &v, 1);
}
/* a trip to a friend, every frame in the world with no trip on its way: to its map, then behind it */
static void friend_step(CPU *c, uint32_t car, long f){
    if (T.spot != F.spot){
        if (F.went){ said(SAID_NOT_FOUND, 0, f); F.name = 0; return; }   /* (its map's trip went elsewhere) */
        int why = why_not(F.spot);
        if (why){ said(SAID_NOT_NOW, why, f); F.name = 0; return; }
        go(c, F.spot, f); F.went = 1; F.since = 0;
        return;
    }
    if (!F.since) F.since = f;
    float p[3]; uint16_t h;
    if (!roam_game_friend_at(F.name, p, &h)){
        if (f - F.since > FRIEND_WAIT){ said(SAID_NOT_FOUND, 0, f); F.name = 0; }
        return;
    }
    float dx = MRF32(car + 0x60u) - p[0], dy = MRF32(car + 0x64u) - p[1];
    if (dx * dx + dy * dy < FRIEND_NEAR_M * FRIEND_NEAR_M){ said(SAID_THERE, 0, f); F.name = 0; return; }
    if (F.went == 2){ said(SAID_THERE, 0, f); F.name = 0; return; }   /* (behind it once: it drove on since) */
    float a = (float)h * (6.28318531f / 65536.f), back[3] = { p[0] - FRIEND_BACK_M * cosf(a), p[1] - FRIEND_BACK_M * sinf(a), p[2] };
    float out[3]; uint16_t oh = h;
    int ok = T.spot ? road_near(back, out) : walls_open_near(p, back, p[0], p[1], out, &oh);
    if (!ok){ said(T.spot ? SAID_NOT_FOUND : SAID_LOCKED, 0, f); F.name = 0; return; }
    int why = why_not(1);                             /* (as a trip to a race track: none with player 2 in) */
    if (why){ said(SAID_NOT_NOW, why, f); F.name = 0; return; }
    go_place(c, out, T.spot ? h : oh, f);
    F.went = 2;
}

/* the status's "travel" (the page's Tracks): in a career's world, or on the way; else 0 (no Tracks row) */
static void publish(int why){
    double v = why < 0 ? 0. : TRAVEL_HAS + why * 256 + T.spot;
    if (v == T.shown) return;
    if (T.slot == -2) T.slot = host_status_slot("travel");
    if (T.slot >= 0){ host_status_set_n(&T.slot, &v, 1); T.shown = v; }
}

void travel_frame(CPU *c){
    long f = autotest_frame();
    static int test_read;
    if (!test_read){ test_read = 1; g_test_noevent = getenv("NFSU2_INPUT_SCRIPT") && getenv("NFSU2_TRAVEL_TEST_NOEVENT"); }
    uint32_t flow = MR32(G_FLOW), track = MR32(G_RACEPARAMS), car = player_car();
    int career = MR32(G_GAMEMODE) == 1u, world = flow == 6u && career && !MR32(G_RACETYPE) && car;
    /* the front end (the pause menu's Quit, a load): the career leaves free roam for good, with the city's place (another
     * profile may load next: nothing of this one carries over) */
    if (flow == 3u && (T.spot || T.going || T.city_ok)){
        if (T.city_ok && (T.spot || T.going || track_map(track))){
            keep_city();
            fprintf(stderr, "[travel] f=%ld the menus: the career keeps the city's place %.1f %.1f %.1f\n", f,
                    (double)T.city[0], (double)T.city[1], (double)T.city[2]);
        }
        T.spot = T.going = T.loaded = T.full = 0; T.city_ok = 0;
    }
    if (T.going && !world) T.going_at = f;            /* (loading: the wait counts in the world only) */
    if (world && T.going && T.loaded){
        uint32_t want = T.going == 1 ? CITY : SPOTS[T.going - 2].track;
        if (track == want){
            T.spot = T.going - 1; T.going = 0;
            roam_game_jumped();                       /* (others who came to the same start line stand there too) */
            fprintf(stderr, "[travel] f=%ld at %s (track %u, %.8s), the car at %.1f %.1f %.1f\n", f,
                    T.spot ? SPOTS[T.spot - 1].name : "the city", track, MR32(G_TRACKINFO) ? (const char *)(g_mem + MR32(G_TRACKINFO) + 0x40u) : "?",
                    (double)MRF32(car + 0x60u), (double)MRF32(car + 0x64u), (double)MRF32(car + 0x68u));
        }
    }
    if (T.going && world && f - T.going_at > GOING_FRAMES){
        fprintf(stderr, "[travel] f=%ld the trip to %s never started: given up\n", f, T.going > 1 ? SPOTS[T.going - 2].name : "the city");
        T.going = T.loaded = T.full = 0;
    }
    if (world && !T.going){
        int k = track_map(track) ? spot_on(track, car) : 0;
        if (T.spot != k && !(k && T.spot && SPOTS[T.spot - 1].track == track)){
            fprintf(stderr, "[travel] f=%ld %s\n", f, k ? "on a track's map: at its spot nearest the car" : "back in the city (the game's own way)");
            T.spot = k;
        }
    }
    if (world && on_track() && T.city_ok && !T.going) keep_city();   /* (a trip's start goes in the same fields) */
    if (F.name && world && !T.going) friend_step(c, car, f);
    int k = __atomic_exchange_n(&g_ask, 0, __ATOMIC_ACQUIRE);
    if (k == ASK_FRIEND){
        int why = why_not(1);
        F.name = __atomic_load_n(&g_ask_name, __ATOMIC_RELAXED); F.spot = __atomic_load_n(&g_ask_spot, __ATOMIC_RELAXED);
        F.since = 0; F.went = 0;
        fprintf(stderr, "[travel] f=%ld to a friend (%08x) at %s\n", f, F.name, F.spot ? SPOTS[F.spot - 1].name : "the city");
        if (why && why != WHY_COOP){ said(SAID_NOT_NOW, why, f); F.name = 0; }
    } else if (k) go(c, k - 1, f);
    publish(career && (flow == 6u || T.going) ? why_not(1) : -1);
}

/* The World Map's key (Player's input 0x602BD0, its event 0xC0) asks the career whether there is a map (0x500B60: none
 * in Quick Race's Free Run, where M does nothing) before it stops the world for the map's screen (race status event
 * 0x12, then 0x606730 shows it, and its close starts the world again); on a track's map the answer is none.  (Later
 * than that, the world stays stopped: the screen that would start it again never comes) */
int travel_hook(CPU *c, uint32_t ip){
    switch (ip){
    case HOOK_SITE(0x00602eef):
        if (on_track()) c->eax &= ~0xffu;
        return 1;
    }
    return 0;
}

/* The outrun challengers' manager (race coordinator +0xC40, Update 0x42EDF0; roam_game.c em_update) asks at its state 1
 * whether an outrun is due (0x41ACE0, thiscall, no arguments: the career's 0x500CB0, then its three tuners' rides set
 * up) and sends the tuners out when so (state 3).  On a track's map the answer is no: the tuners stay in the city, and
 * the manager waits at 1 until the car is back there */
void sub_0041ace0_orig(CPU *c);
void sub_0041ace0(CPU *c){
    if (on_track()){ c->eax &= ~0xffu; POP32(c->eip); return; }
    sub_0041ace0_orig(c);
}

/* "travel near X Y":the loaded map's three road points nearest X Y with their heights (the road table 0x88A334, 2048:
 * a road's nodes from +0x8C, 0x38 bytes, x y and the height at +0x12 in 1/8 m, as walls.c reads them), for a spot's
 * height; roads over roads show as points at the same place with other heights */
static void near_roads(long f, float x, float y){
    float bd[3] = { 1e30f, 1e30f, 1e30f }, bp[3][3] = {{0}};
    for (uint32_t i = 0; i < 2048u; i++){
        uint32_t r = guest_rd32(0x0088a334u + 4u * i);
        if (!guest_ok(r, 0x8cu) || (int16_t)MR16(r + 0xau) != (int)i) continue;
        int n = (int16_t)MR16(r + 0x34u);
        for (int j = 0; j < n && j < 4096 && guest_ok(r + 0x8cu + 0x38u * (uint32_t)j, 0x38u); j++){
            uint32_t nd = r + 0x8cu + 0x38u * (uint32_t)j;
            float dx = MRF32(nd) - x, dy = MRF32(nd + 4u) - y, d = dx * dx + dy * dy;
            for (int s = 0; s < 3; s++) if (d < bd[s]){
                for (int t = 2; t > s; t--){ bd[t] = bd[t - 1]; memcpy(bp[t], bp[t - 1], sizeof bp[t]); }
                bd[s] = d; bp[s][0] = MRF32(nd); bp[s][1] = MRF32(nd + 4u); bp[s][2] = (float)(int16_t)MR16(nd + 0x12u) / 8.f;
                break;
            }
        }
    }
    fprintf(stderr, "[travel] f=%ld near %.1f %.1f (track %u):", f, (double)x, (double)y, MR32(G_RACEPARAMS));
    for (int s = 0; s < 3; s++) if (bd[s] < 1e29f)
        fprintf(stderr, " %.1f %.1f height %.1f (%.1f m)", (double)bp[s][0], (double)bp[s][1], (double)bp[s][2], (double)sqrtf(bd[s]));
    fputc('\n', stderr);
}

/* script command (tests): "travel N" a trip to spot N (1..), "travel city" back, "travel where" where the car is,
 * "travel near X Y" the road points there (near_roads), "travel friend NAME [SPOT]" to that player (at that spot) */
int travel_cmd(const char *args){
    long f = autotest_frame();
    char a[32] = {0};
    sscanf(args, "%31s", a);
    if (!strcasecmp(a, "where")){
        uint32_t car = player_car(), ti = MR32(G_TRACKINFO);
        fprintf(stderr, "[travel] f=%ld where: %s, track %u (%.8s), why not %s; the car at %.1f %.1f %.1f heading 0x%04x "
                        "velocity %.1f %.1f %.1f; the kept spot %.1f %.1f %.1f\n", f,
                T.spot ? SPOTS[T.spot - 1].name : "the city", MR32(G_RACEPARAMS), ti ? (const char *)(g_mem + ti + 0x40u) : "?",
                WHY[why_not(1)], car ? (double)MRF32(car + 0x60u) : 0., car ? (double)MRF32(car + 0x64u) : 0.,
                car ? (double)MRF32(car + 0x68u) : 0., car ? MR16(car + 0x40cu) : 0u, car ? (double)MRF32(car + 0x70u) : 0.,
                car ? (double)MRF32(car + 0x74u) : 0., car ? (double)MRF32(car + 0x78u) : 0.,
                (double)MRF32(G_CAREER + C_SPOT), (double)MRF32(G_CAREER + C_SPOT + 4u), (double)MRF32(G_CAREER + C_SPOT + 8u));
        return 1;
    }
    if (!strcasecmp(a, "near")){ float x = 0, y = 0; sscanf(args, "%*s %f %f", &x, &y); near_roads(f, x, y); return 1; }
    if (!strcasecmp(a, "friend")){                    /* "travel friend NAME [SPOT]": as the page's Friends, Join */
        char nm[40] = {0}; int k = 0;
        if (sscanf(args, "%*s %39s %d", nm, &k) < 1){ fprintf(stderr, "[travel] f=%ld travel friend: NAME [SPOT]\n", f); return 1; }
        travel_ask_friend(rc_room_hash(nm), k);
        return 1;
    }
    int k = !strcasecmp(a, "city") ? 0 : atoi(a);
    if (k < 0 || k > NSPOTS){ fprintf(stderr, "[travel] f=%ld travel: N (1..%d), city, where or near X Y\n", f, NSPOTS); return 1; }
    travel_ask(k);
    return 1;
}
