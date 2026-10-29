/* racenet.c -- how the other players' cars came in during a network race
 * (the game's Online and LAN races), written to the log when the race ends.
 *
 * Players report cars that "freeze and then move two frames" or "move a bit
 * left, a bit right".  Each cause wants another fix: a race link that held
 * the datagrams (TCP waits for a lost segment, then hands over everything at
 * once), a player whose game ran slowly or stalled, or the game's own way of
 * showing the cars.  So when its player finishes the race (or leaves it)
 * each game writes:
 *   - its own frames while racing: the rate, the frames over 50 ms, the
 *     longest;
 *   - each race link (the addresses the game's UDP 3658 heard from: the
 *     creator for a joiner, every joiner for the creator, the race being a
 *     star around the game's creator): datagrams, the longest wait, the
 *     waits of 250 ms or more and how many datagrams came at once ending
 *     them, against how many came at once otherwise (racenet.h, net_lan.c);
 *   - each other car: its updates (the game's car states, 0x5F8900) a
 *     second and how many of them came straight from its game (below), the
 *     longest wait, the waits over 100, 250 and 500 ms, its jumps as shown
 *     here and as the game had them (a frame whose move differs from the
 *     frame before's by over 0.5 m and over 2 m), the corrections faded and
 *     the ones shown (below), the "hits" of a far car passed (sub_005f8d40),
 *     its states' age by this game's race clock (mean, least, most: the
 *     way's delay), and that player's own frame rate and race clock, which
 *     every game sends the others every FS_MS.
 *
 * It also shows the cars more smoothly than the game alone:
 *   - a car far from the player no longer loses its predictions to a "hit"
 *     of its own physics (sub_005f8d40; it stood still for a frame or two,
 *     then stepped);
 *   - straight from their games: a joiner's car went joiner -> creator ->
 *     joiner, two ways, then the game predicted it that far ahead.  Two
 *     players' logs (2026-10-25/26, about 300 ms a way) had the creator's car
 *     323 ms old to a joiner and the other joiners' 626-721 ms, with 74-192
 *     jumps over 0.5 m a minute each (18-43 m the largest); the same player
 *     as the creator saw 1-17.  Now every joiner also sends its car states
 *     to the other joiners itself (sub_00585d50, "N2CS" on the horn's port),
 *     and they hand them to the game as if the creator had passed them on;
 *     the game keeps whichever copy comes first (below).  A joiner learns
 *     the others from the creator's list ("N2PL"); a game of an older
 *     version sends no list and drops these, so in a race with an older
 *     creator nothing changes, and an older joiner's car still comes the
 *     long way;
 *   - corrections fade: when a state pulls a car to where its owner had
 *     it, the step is drawn as an offset that fades in about 0.2 s
 *     (critically damped: no kick) instead of a snap; over FADE_MAX (after a
 *     stall) the car is shown where it is.  Only where the car is drawn:
 *     the offset goes on after the world's step, for the render, and the
 *     car's own place goes back before the next step (racenet_prestep), so
 *     the game's physics, collisions and predictions keep its own.
 * NFSU2_RACE_SMOOTH=0 turns all of it off (the game's own, for A/B tests);
 * NFSU2_RACE_DIRECT=0 only the states straight from their games,
 * NFSU2_RACE_FADE=0 only the fading.
 *
 * Measured 2026-10-28 (web/tools/lan-pair.mjs, three headless games on one
 * PC at 27-48 fps, the switch's --lan-impair leg=150,legjitter=20: about
 * 300 ms a way; jumps over 0.5 m a minute, and the largest): a joiner's
 * view of the other joiner's car was 656 ms old, 87 (30 m); straight from
 * its game 321 ms, 9.3 (5.6 m) as the game had it, 1.4 (0.8 m) as shown
 * with the fade.  The creator's car to a joiner 25 (5.1 m), shown 1.6
 * (1.2 m); nothing over 2 m shown.  Four players: joiner to joiner 0.8
 * (1.6 m).  With 1.5 s stalls on every player's way (stall=0.05,
 * stallms=1500) joiner to joiner 76 (21 m) became 2.5, but what comes after
 * a stall is still shown where it lands (up to 31 m from the prediction).
 * Races with a game of the older version on either side finished as before.
 *
 * The game (SPEED2.EXE v1.2 NA, static reading):
 *  - The car states (MSG_U_BI_CARSPAM) go out from a scheduler with tiers of
 *    20, 10 and 5 a second (0x583710, the table 0x7FBE68); the creator
 *    passes the joiners' on.
 *  - 0x5FD360 decodes one into a remote car's controller (its flags: 1 the
 *    position, orientation and acceleration, 2 the velocity and spin, 4 and
 *    8 more; a field not sent keeps what the controller held) and calls 0x5F8900
 *    (thiscall, ret 4: the controller, the state): the state's controls go
 *    into the car ([ctl+0x10], the world's car), and a prediction from the
 *    pool [0x89012C] ("ExtrapolatedStateSlotPool", made by the World
 *    constructor for network races only) joins the car's list, at most 10,
 *    each living the link's latency + 0.1 + 0.333 s.  Every frame 0x5F8AF0
 *    blends the live ones, the newest weighed most; with none left the car
 *    gets fixed controls until the next state.
 *
 * The car states (static reading):
 *  - The sender 0x587830 (every frame the scheduler allows one) writes a bit
 *    stream (0x581D00: most significant bit first; +0x400 the bits written,
 *    +0x404 those read): 8 bits the message (2, MSG_U_BI_CARSPAM), its race
 *    clock (32, a float), its slot ([0x8691B4] bits, plus [0x8691B8]), its
 *    real-time clock (32, a float), the scheduler's flags (8), then the car
 *    (0x5F9930: the car's controller +0x918, its vtable 0x7A63D8 +4 is
 *    0x5FCD60).  0x585D50 (cdecl: 2, the stream, 0) sends ceil(bits / 8)
 *    bytes to the creator.
 *  - The creator (its handler 0x58F020, message 2 0x5895C0) takes the slot
 *    from the connection, not the message, decodes the car, then passes the
 *    message's bytes unchanged to every other joiner (0x586120), and
 *    answers the sender with its own car and the clock (message 4).
 *  - A joiner's handler (0x58E900, cdecl: the bytes, their length, 0, from
 *    [0x866750]) reads the message; 2 is 0x58BFB0 -> 0x5890E0: the slot, the
 *    real-time clock, the flags; a state no newer than the last one of that
 *    player ([player +0x26C], the sender's real-time clock) is dropped, else
 *    0x5F9840 hands it to the car's controller (vtable +0xC, 0x5FD360, which
 *    rejects one from the future and calls 0x5F8900).  A state of the
 *    creator's car (slot [0x7FBE64]) is answered (message 3, the clock).
 *    So a copy straight from the joiner's game and the creator's copy carry
 *    the same clock, and the game takes the first one to come.
 *
 * Frame rates go on the horn's port (racehorn.c's HORN_PORT, whose receiver
 * hands "N2FS" messages here): "N2FS", version 1, flags (2: relayed), the
 * frames a second x 10 (u16, little-endian), the longest frame in ms (u16),
 * the frames over 50 ms (u8), 0, its race clock (u32, 1/4000 s), then the
 * name the player races under (16 bytes, as racehorn.c's).  The creator
 * passes each joiner's on to the other joiners, marked relayed.  A game of
 * an older version sends none and drops these.  Also there:
 *   "N2CS", version 1, 0, the length (u16), then the bit stream's bytes as
 *   0x585D50 sent them: a joiner's car state, to every joiner on the list;
 *   "N2PL", version 1, 0, the count (u8), then that many addresses (u32,
 *   host order): the creator's list of the other joiners, to each joiner
 *   every PL_MS.  Taken only from the race's creator (a joiner's one race
 *   peer), and car states only from the addresses on its list, only for a
 *   slot that is neither the creator's nor ours.
 *
 * The race clocks: a joiner's follows the creator's (0x5891B0, about 20
 * samples a second, each the creator's clock plus half the round trip), and
 * every car state is stamped with its sender's.  "their race clock ... off
 * ours" is the N2FS clock against this game's, the way's delay in it: tens
 * of ms when they agree.  (tools/lan_trace.py lines instances up by their
 * wall clocks, which in the browser can be a second or more apart: that
 * looked like a joiner's race clock 1.2-1.6 s behind for a whole race, while
 * this figure said -8 ms.)
 */
#include "cpu.h"
#include "racenet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define G_FLOW       0x008654a4u   /* 6: in the world */
#define G_NETMODE    0x00850078u   /* 8 Online, 0x200 LAN (1 career) */
#define G_NET_HOST   0x00866a14u   /* host race state, > 0: this game created the race */
#define G_WORLD      0x00890080u   /* World*: +0 the cars' list (a car's first two dwords are its node) */
#define G_PLAYER1    0x008900acu   /* Player*: +4 car */
#define G_WORLDDT    0x00865154u   /* float: this frame's world dt (s) */
#define G_RACE       0x00890118u   /* the race coordinator*: +0 state, +0x24 racers, +0xBD0 racer pointers */
#define RC_RACING    4u            /*   (a network race stays at 4 behind the results, until it is left) */
#define RCR_DONE     0x10u         /* a racer: +0 its car, +0x10 u16 how it finished, 0 not yet */
#define G_NETPLAYERS 0x0089cf50u   /* the network race's players: 6 pointers, name at +0x12 */
#define NP_NAME      0x12u
#define CTL_CAR      0x10u         /* a remote car's controller: its world car */
#define CTL_PREV_V   0x140u        /*   the car's velocity as the last blend left it (3 floats) */
#define CTL_HIT      0x184u        /*   world time (1/4000 s) of the last hit it saw */
#define ST_TIME      0x7cu         /* a car state: float, the sender's race clock (s) */
#define G_WORLDTIME  0x0086518cu   /* the race clock (WorldTimer, 1/4000 s), slaved to the creator's */
#define CAR_POS      0x60u         /* a car: position, velocity (3 floats each) */
#define CAR_VEL      0x70u
#define HIT_M        80.f          /* m: a remote car farther from the player's is out of reach of a hit here */
#define HORN_PORT    3659u         /* racehorn.c's */
#define PEER_MS      5000u
#define FS_MS        2000u
#define FS_N         (16 + NAME_N)
#define NAME_N       16
#define NCARS        8
#define NLINKS       16
#define SLOW_US      50000u        /* a frame longer than this is a stall */
/* the car states (above) */
#define G_MYSLOT     0x007fbe5cu   /* a joiner's slot, and the creator's (the creator's welcome, 0x58BFE0) */
#define G_HOSTSLOT   0x007fbe64u
#define G_SLOTBITS   0x008691b4u   /* a slot's bits in a message, and the least slot (0x583F80) */
#define G_SLOTMIN    0x008691b8u
#define FN_JOIN_MSG  0x0058e900u   /* a joiner's handler of the creator's messages (cdecl: the bytes, the length, 0) */
#define HOOK_MAINLOOP 0x00581475u  /* the return address of guest calls from the main-loop hook */
#define MSG_CARSPAM  2u
#define BS_BYTES     0x400u        /* a bit stream: the bytes, then +0x400 the bits written */
#define CS_HDR       8u
#define PL_MS        1000u         /* the creator's list of the joiners, every PL_MS */
#define PL_STALE_MS  5000u         /*   and the list a joiner goes by, at most this old */
#define NPL          8
/* the fade (above) */
#define FADE_MIN     0.15f         /* m: a frame's move this much off the car's own (its velocity, its last move) is a correction */
#define FADE_MAX     10.f          /* m: a correction (with what still shows of the last ones) over this is shown */
#define FADE_W       20.f          /* 1/s: the fade, critically damped: 9 % left after 0.2 s */

/* net_lan.c in the browser (racehorn.c has the stand-ins for the others) */
int vlan_rt_send(uint16_t port, uint32_t ip_h, const void *p, int n);
int vlan_race_peers(uint32_t *ip_h, int max, uint32_t within_ms);
__attribute__((weak)) int vlan_race_links(RaceLink *o, int max, int reset){ (void)o; (void)max; (void)reset; return 0; }

static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n); }
static uint32_t rd32(uint32_t a){ return ok(a, 3) ? MR32(a) : 0u; }
static void rd3(uint32_t a, float *o){ o[0] = MRF32(a); o[1] = MRF32(a + 4u); o[2] = MRF32(a + 8u); }
static uint64_t now_us(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
/* the name of the player whose car this is, as racehorn.c's car_name */
static void car_name(uint32_t car, char *o){
    int i = ok(car, 0x9e0) ? (int16_t)MR16(car + 0xau) : -1;
    uint32_t pl = i >= 0 && i < 6 ? rd32(G_NETPLAYERS + 4u * (uint32_t)i) : 0;
    int n = 0;
    if (ok(pl, NP_NAME + NAME_N))
        for (; n < NAME_N - 1; n++){ uint8_t ch = MR8(pl + NP_NAME + (uint32_t)n); if (ch < 0x20 || ch > 0x7e) break; o[n] = (char)ch; }
    o[n] = 0;
}

/* this race (R.on while the coordinator says racing) */
static struct {
    int on, host;
    char me[NAME_N];
    uint64_t t0, prev;                               /* its start, the last frame */
    uint32_t frames, slow; uint64_t longest;
    uint64_t w0; uint32_t wframes, wslow; uint64_t wlongest;   /* since our last N2FS */
    uint32_t cs_sent, cs_came, cs_refused;           /* car states straight to and from the other joiners */
} R;
/* a car's jumps: a frame whose move differs from the frame before's */
typedef struct { float p[3], d[3], step; int have, jumped; uint32_t jumps, big; float max; } Jumps;
/* the other cars, by their world car */
typedef struct {
    uint32_t car; char name[NAME_N];
    uint32_t n, direct, g100, g250, g500; uint64_t first, last, gap_max;
    Jumps shown, game;                                /* as drawn, and as the game had it */
    uint32_t faded, kept;                             /* corrections faded, and shown (over FADE_MAX) */
    uint32_t unhit;                                   /* far "hits" passed (sub_005f8d40) */
    float age_min, age_max; double age_sum;           /* its states' age by our race clock (s) */
} Car;
static Car g_car[NCARS];
/* the others' frame rates, by name */
typedef struct { char name[NAME_N]; uint32_t n, slow, longest_ms; float sum, lo; double clock; } Fps;
static Fps g_fps[NCARS];

static Car *car_of(uint32_t car, int add){
    for (int i = 0; i < NCARS; i++) if (g_car[i].car == car) return &g_car[i];
    if (!add) return 0;
    for (int i = 0; i < NCARS; i++) if (!g_car[i].car){ memset(&g_car[i], 0, sizeof g_car[i]); g_car[i].car = car; return &g_car[i]; }
    return 0;
}

/* NFSU2_RACE_SMOOTH=0 shows the other cars as the game itself does (A/B
 * tests); NFSU2_RACE_DIRECT=0 and NFSU2_RACE_FADE=0 leave out one part */
static int env_on(const char *k){ const char *e = getenv(k); return !(e && e[0] == '0'); }
static int smooth_on(void){
    static int on = -1;
    if (on < 0) on = env_on("NFSU2_RACE_SMOOTH");
    return on;
}
static int direct_on(void){
    static int on = -1;
    if (on < 0) on = smooth_on() && env_on("NFSU2_RACE_DIRECT");
    return on;
}
static int fade_on(void){
    static int on = -1;
    if (on < 0) on = smooth_on() && env_on("NFSU2_RACE_FADE");
    return on;
}
/* a network race runs here (also after our own player finished: the
 * others still race, and still come) */
static int in_race(void){
    uint32_t w = rd32(G_WORLD), rc = rd32(G_RACE);
    return rd32(G_FLOW) == 6u && (rd32(G_NETMODE) & 0x208u) && ok(w, 0x80) && rd32(rc) == RC_RACING;
}
static int is_host(void){ return (int32_t)rd32(G_NET_HOST) > 0; }
static uint32_t ms_now(void){ return (uint32_t)(now_us() / 1000u); }
/* the car is farther than HIT_M from the player's */
static int far_from_me(uint32_t car){
    uint32_t pl = rd32(G_PLAYER1), me = pl ? rd32(pl + 4u) : 0;
    if (!ok(me, CAR_POS + 12u) || !ok(car, CAR_VEL + 12u) || car == me) return 0;
    float a[3], b[3], d2 = 0;
    rd3(me + CAR_POS, a); rd3(car + CAR_POS, b);
    for (int i = 0; i < 3; i++) d2 += (a[i] - b[i]) * (a[i] - b[i]);
    return __builtin_isfinite(d2) && d2 > HIT_M * HIT_M;
}
/* A remote car's predictions are moved on every world step by 0x5F8D40
 * (thiscall: the controller, the step), which first compares the car's
 * velocity with the one the last blend left it ([ctl+0x140]): a change of
 * 4 m/s or more is taken for a hit here, stamped [ctl+0x184], and the next
 * blend drops every prediction made from a state older than that (1 m/s
 * puts the car's own velocity and spin into the controller).  Far from the
 * player (seen at 220-335 m) the car's own physics changes its velocity that
 * much now and then: hit-marked in 5 % of the frames, its predictions gone
 * until the next state, the car stood still for a frame or two and then
 * stepped, 400-950 times a race to the creator's eyes.  Beyond HIT_M no hit
 * of this game can touch the car, and a hit of its own comes with its
 * owner's states, so its check sees no change. */
void sub_005f8d40_orig(CPU *c);
void sub_005f8d40(CPU *c){
    uint32_t ctl = c->ecx, car = ok(ctl, CTL_HIT + 4u) ? rd32(ctl + CTL_CAR) : 0;
    if (car && smooth_on() && far_from_me(car)){
        float v[3], pv[3], d2 = 0;
        rd3(car + CAR_VEL, v); rd3(ctl + CTL_PREV_V, pv);
        for (int i = 0; i < 3; i++) d2 += (v[i] - pv[i]) * (v[i] - pv[i]);
        Car *e = R.on ? car_of(car, 0) : 0;
        if (e && d2 >= 16.f) e->unhit++;
        for (int i = 0; i < 3; i++) MRF32(ctl + CTL_PREV_V + 4u * (uint32_t)i) = v[i];
    }
    sub_005f8d40_orig(c);
}

/* the fade's cars: the remote ones (they get states), by their world car */
typedef struct {
    uint32_t car;
    float s[3], v[3], m[3], h; int have;      /* the car as the game had it last frame: place, velocity, move, its time */
    float o[3], r[3];                         /* the offset drawn, and its rate */
    uint32_t put60[3], put50[3], was60[3], was50[3]; int put;   /* what the render got, and the game's own */
} Fade;
static Fade g_fade[NCARS];
static uint32_t g_fade_world;
static Fade *fade_of(uint32_t car, int add){
    for (int i = 0; i < NCARS; i++) if (g_fade[i].car == car) return &g_fade[i];
    if (!add) return 0;
    for (int i = 0; i < NCARS; i++) if (!g_fade[i].car){ memset(&g_fade[i], 0, sizeof g_fade[i]); g_fade[i].car = car; return &g_fade[i]; }
    return 0;
}
static int g_direct;                          /* the state being handled came straight from its game */

/* a car state came for a remote car (thiscall: the controller, the state) */
void sub_005f8900_orig(CPU *c);
void sub_005f8900(CPU *c){
    uint32_t ctl = c->ecx, st = MR32(c->esp + 4u), car = ok(ctl, CTL_CAR + 4u) ? MR32(ctl + CTL_CAR) : 0;
    Car *e = car && R.on ? car_of(car, 1) : 0;
    if (car && g_fade_world && rd32(G_WORLD) == g_fade_world) fade_of(car, 1);
    if (e){
        e->direct += g_direct;
        uint64_t t = now_us();
        if (e->n){
            uint64_t g = t - e->last;
            if (g > e->gap_max) e->gap_max = g;
            e->g100 += g > 100000u; e->g250 += g > 250000u; e->g500 += g > 500000u;
        } else e->first = t;
        if (ok(st, ST_TIME + 4u)){                        /* the state's age by this game's race clock */
            float age = (float)rd32(G_WORLDTIME) * 0.00025f - MRF32(st + ST_TIME);
            if (__builtin_isfinite(age)){
                if (!e->n || age < e->age_min) e->age_min = age;
                if (!e->n || age > e->age_max) e->age_max = age;
                e->age_sum += age;
            }
        }
        e->n++; e->last = t;
        if (!e->name[0]) car_name(car, e->name);
    }
    sub_005f8900_orig(c);
}

/* ---- car states straight from their games (above) ---- */
static struct { uint32_t from, at, n, ip[NPL]; } g_pl;   /* a joiner: the creator's list (host order), when it came */
static uint32_t g_pl_sent;                              /* the creator: when it last sent it */

/* A message goes to the creator (cdecl: its kind for the game's statistics,
 * the bit stream, 0): a joiner's car state (0x587830's) also goes straight
 * to the other joiners on the creator's list. */
void sub_00585d50_orig(CPU *c);
void sub_00585d50(CPU *c){
    uint32_t kind = MR32(c->esp + 4u), bs = MR32(c->esp + 8u);
    if (kind == MSG_CARSPAM && direct_on() && g_pl.n && ms_now() - g_pl.at <= PL_STALE_MS && !is_host() && in_race() &&
        ok(bs, BS_BYTES + 4u)){
        uint32_t bits = MR32(bs + BS_BYTES), n = (bits + 7u) / 8u;
        if (bits >= 48u && n <= BS_BYTES && MR8(bs) == MSG_CARSPAM){
            static uint8_t m[CS_HDR + BS_BYTES];
            memcpy(m, "N2CS", 4); m[4] = 1; m[5] = 0; m[6] = (uint8_t)n; m[7] = (uint8_t)(n >> 8);
            for (uint32_t i = 0; i < n; i++) m[CS_HDR + i] = MR8(bs + i);
            for (uint32_t k = 0; k < g_pl.n; k++) if (!vlan_rt_send(HORN_PORT, g_pl.ip[k], m, (int)(CS_HDR + n))) R.cs_sent++;
        }
    }
    sub_00585d50_orig(c);
}
/* the creator's list to each joiner: the others (every PL_MS, racenet_frame) */
static void list_send(void){
    uint32_t peers[NPL];
    int n = vlan_race_peers(peers, NPL, PEER_MS);
    for (int i = 0; i < n; i++){
        uint8_t m[8 + 4 * NPL];
        int k = 0;
        memcpy(m, "N2PL", 4); m[4] = 1; m[5] = 0; m[7] = 0;
        for (int j = 0; j < n; j++){
            if (j == i) continue;
            for (int b = 0; b < 4; b++) m[8 + 4 * k + b] = (uint8_t)(peers[j] >> (8 * b));
            k++;
        }
        m[6] = (uint8_t)k;
        vlan_rt_send(HORN_PORT, peers[i], m, 8 + 4 * k);
    }
}
/* an N2PL from the horn's port (racehorn.c): only from the race's creator,
 * a joiner's one race peer */
void racenet_list(const uint8_t *m, int n, uint32_t from){
    if (n < 8 || m[4] != 1 || m[6] > NPL || n < 8 + 4 * m[6] || is_host()) return;
    uint32_t peers[4];
    int np = vlan_race_peers(peers, 4, PEER_MS), creator = 0;
    for (int i = 0; i < np; i++) creator |= peers[i] == from;
    if (!creator) return;
    g_pl.from = from; g_pl.at = ms_now(); g_pl.n = 0;
    for (int k = 0; k < m[6]; k++){
        const uint8_t *q = m + 8 + 4 * k;
        uint32_t a = (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
        if (a && a != from) g_pl.ip[g_pl.n++] = a;
    }
}
/* bits of a bit stream from pos on, most significant first (0x581D90) */
static uint32_t bits_at(const uint8_t *b, uint32_t pos, uint32_t nb){
    uint32_t v = 0;
    for (uint32_t i = 0; i < nb; i++, pos++) v = v << 1 | (uint32_t)(b[pos >> 3] >> (7u - (pos & 7u)) & 1u);
    return v;
}
/* An N2CS from the horn's port (racehorn.c, on the game thread): to the
 * game's handler, as the creator's copy goes.  Only from a joiner on the
 * creator's list, only a car state, only for a slot that is neither ours nor
 * the creator's (whose states the game answers). */
void racenet_carspam(CPU *c, const uint8_t *m, int n, uint32_t from){
    if (!direct_on() || n < (int)CS_HDR || m[4] != 1 || is_host() || !in_race()) return;
    uint32_t len = (uint32_t)m[6] | (uint32_t)m[7] << 8, nb = rd32(G_SLOTBITS), known = 0;
    for (uint32_t k = 0; k < g_pl.n; k++) known |= g_pl.ip[k] == from;
    if (!known || ms_now() - g_pl.at > PL_STALE_MS || len < 6u || len > BS_BYTES || (uint32_t)n < CS_HDR + len ||
        m[CS_HDR] != MSG_CARSPAM || nb < 1u || nb > 8u || 40u + nb > 8u * len){ R.cs_refused++; return; }
    uint32_t slot = bits_at(m + CS_HDR, 40u, nb) + rd32(G_SLOTMIN);
    if (slot >= 6u || slot == rd32(G_MYSLOT) || slot == rd32(G_HOSTSLOT)){ R.cs_refused++; return; }
    R.cs_came++;
    /* the bytes below the hook's frame, the call below them */
    CPU save = *c;
    uint32_t buf = (c->esp - 0x1000u - len) & ~15u;
    for (uint32_t i = 0; i < len; i++) MR8(buf + i) = m[CS_HDR + i];
    c->esp = buf - 64u;
    PUSH32(0); PUSH32(len); PUSH32(buf);
    PUSH32(HOOK_MAINLOOP);
    g_direct = 1;
    dispatch_indirect(c, FN_JOIN_MSG);
    g_direct = 0;
    *c = save;
}

/* ---- the fade (above) ---- */
/* the car is on the world's list still */
static int in_world(uint32_t w, uint32_t car){
    uint32_t x = rd32(w);
    for (int k = 0; x && x != w && k < 64 && ok(x, 0x9e0); k++, x = rd32(x)) if (x == car) return 1;
    return 0;
}
/* the cars' own places back: before the world's step, and before a frame
 * that had none puts its offsets on again.  A place the game changed since
 * (a reset) is the game's, the offset goes. */
static void fade_back(void){
    uint32_t w = rd32(G_WORLD);
    for (int i = 0; i < NCARS; i++){
        Fade *f = &g_fade[i];
        if (!f->put) continue;
        f->put = 0;
        if (w != g_fade_world || !in_world(w, f->car)) continue;
        int same = 1;
        for (uint32_t k = 0; k < 3; k++) same &= MR32(f->car + CAR_POS + 4u * k) == f->put60[k] && MR32(f->car + 0x50u + 4u * k) == f->put50[k];
        if (!same){ memset(f->o, 0, sizeof f->o); memset(f->r, 0, sizeof f->r); continue; }
        for (uint32_t k = 0; k < 3; k++){ MR32(f->car + CAR_POS + 4u * k) = f->was60[k]; MR32(f->car + 0x50u + 4u * k) = f->was50[k]; }
    }
}
void racenet_prestep(CPU *c){ (void)c; fade_back(); }
/* x(t) = (o + (r + w o) t) e^-wt over h: a critically damped return to 0 */
static void fade_step(float *o, float *r, float h){
    float e = __builtin_expf(-FADE_W * h);
    for (int k = 0; k < 3; k++){
        float b = r[k] + FADE_W * o[k];
        o[k] = (o[k] + b * h) * e; r[k] = (r[k] - FADE_W * b * h) * e;
    }
}
static float len2(const float *a){ return a[0] * a[0] + a[1] * a[1] + a[2] * a[2]; }
/* After the world's step, before the render (racenet_frame): each remote
 * car's move this frame against its own (its velocities' mean times the
 * step, or its last move: the nearer of the two; a car's physics and its
 * hits move it so within a few cm); a move off both by FADE_MIN is a
 * correction, and the offset takes it back, to fade from there. */
static void fade_frame(void){
    uint32_t w = rd32(G_WORLD);
    int racing = in_race();
    if (w != g_fade_world || !racing){
        fade_back();
        memset(g_fade, 0, sizeof g_fade);
        g_fade_world = racing ? w : 0;
        return;
    }
    fade_back();
    float h = MRF32(G_WORLDDT);
    if (!(h > 0.f && h < 0.25f)) h = 0.f;                /* paused, or loading: nothing moved */
    uint32_t pl = rd32(G_PLAYER1), me = pl ? rd32(pl + 4u) : 0;
    for (int i = 0; i < NCARS; i++){
        Fade *f = &g_fade[i];
        if (!f->car) continue;
        if (f->car == me || !in_world(w, f->car)){ f->car = 0; continue; }
        float s[3], v[3], mv[3], x[3], y[3];
        rd3(f->car + CAR_POS, s); rd3(f->car + CAR_VEL, v);
        if (!__builtin_isfinite(len2(s)) || !__builtin_isfinite(len2(v))){ f->have = 0; continue; }
        for (int k = 0; k < 3; k++) mv[k] = f->have ? s[k] - f->s[k] : 0.f;
        if (f->have && h > 0.f){
            fade_step(f->o, f->r, h);
            float q = f->h > 0.f ? h / f->h : 1.f;
            for (int k = 0; k < 3; k++){ x[k] = mv[k] - 0.5f * (f->v[k] + v[k]) * h; y[k] = mv[k] - f->m[k] * q; }
            float *d = len2(x) < len2(y) ? x : y;
            Car *e = R.on ? car_of(f->car, 0) : 0;
            if (fade_on() && len2(d) > FADE_MIN * FADE_MIN){
                for (int k = 0; k < 3; k++){ f->o[k] -= d[k]; mv[k] -= d[k]; }     /* (its own move, for the next frame) */
                if (len2(f->o) > FADE_MAX * FADE_MAX){ memset(f->o, 0, sizeof f->o); memset(f->r, 0, sizeof f->r); if (e) e->kept++; }
                else if (e) e->faded++;
            }
        }
        if (h > 0.f){ memcpy(f->m, mv, sizeof mv); f->h = h; }
        memcpy(f->s, s, sizeof s); memcpy(f->v, v, sizeof v); f->have = 1;
        if (!fade_on() || len2(f->o) < 1e-6f) continue;
        for (uint32_t k = 0; k < 3; k++){
            uint32_t a = f->car + CAR_POS + 4u * k, b = f->car + 0x50u + 4u * k;
            f->was60[k] = MR32(a); f->was50[k] = MR32(b);
            MRF32(a) = MRF32(a) + f->o[k]; MRF32(b) = MRF32(b) + f->o[k];
            f->put60[k] = MR32(a); f->put50[k] = MR32(b);
        }
        f->put = 1;
    }
}

static void send_all(const uint8_t *m, uint32_t except){
    uint32_t peers[16];
    int n = vlan_race_peers(peers, 16, PEER_MS);
    for (int i = 0; i < n; i++) if (peers[i] != except) vlan_rt_send(HORN_PORT, peers[i], m, FS_N);
}
/* an N2FS from the horn's port (racehorn.c) */
void racenet_heard(uint8_t *m, int n, uint32_t from){
    if (n < FS_N || memcmp(m, "N2FS", 4) || m[4] != 1) return;
    char name[NAME_N];
    int k = 0;
    for (; k < NAME_N - 1 && m[16 + k] >= 0x20 && m[16 + k] <= 0x7e; k++) name[k] = (char)m[16 + k];
    name[k] = 0;
    if (!name[0]) return;
    if (!(m[5] & 2)){ m[5] |= 2; send_all(m, from); }   /* the creator passes it on */
    if (!R.on) return;
    int j = 0;
    while (j < NCARS && g_fps[j].name[0] && strcmp(g_fps[j].name, name)) j++;
    if (j == NCARS) return;
    Fps *f = &g_fps[j];
    float fps = (float)(m[6] | m[7] << 8) / 10.f;
    uint32_t longest = (uint32_t)(m[8] | m[9] << 8);
    if (!f->name[0]){ memcpy(f->name, name, NAME_N); f->lo = fps; }
    f->n++; f->sum += fps; if (fps < f->lo) f->lo = fps;
    f->slow += m[10]; if (longest > f->longest_ms) f->longest_ms = longest;
    uint32_t theirs = (uint32_t)m[12] | (uint32_t)m[13] << 8 | (uint32_t)m[14] << 16 | (uint32_t)m[15] << 24;
    f->clock += ((double)theirs - (double)rd32(G_WORLDTIME)) / 4.0;   /* ms, the way's delay in it */
}

/* this game's player has finished the race (or left it: the coordinator's
 * own racer entries) */
static int finished(uint32_t rc, uint32_t me){
    uint32_t n = rd32(rc + 0x24u);
    for (uint32_t i = 0; i < n && i < 8; i++){
        uint32_t r = rd32(rc + 0xbd0u + 4u * i);
        if (r && rd32(r) == me) return ok(r, RCR_DONE + 2u) && MR16(r + RCR_DONE) != 0;
    }
    return 0;
}
static void begin(uint64_t t, uint32_t me){
    memset(&R, 0, sizeof R);
    memset(g_car, 0, sizeof g_car);
    memset(g_fps, 0, sizeof g_fps);
    R.on = 1; R.t0 = R.w0 = t;
    R.host = (int32_t)rd32(G_NET_HOST) > 0;
    car_name(me, R.me);
    vlan_race_links(0, 0, 1);
    fprintf(stderr, "[racenet] race begins: me %s, %s\n", R.me[0] ? R.me : "(no name)",
            R.host ? "the game's creator" : "a joiner (every car comes through the creator)");
}
static void ip_str(uint32_t h, char *o){ snprintf(o, 16, "%u.%u.%u.%u", h >> 24, h >> 16 & 255u, h >> 8 & 255u, h & 255u); }
static void end(uint64_t t){
    double s = (double)(t - R.t0) / 1e6;
    fprintf(stderr, "[racenet] race over for me after %.1f s: %u frames, %.1f fps, %u over 50 ms (longest %.0f ms)\n",
            s, R.frames, s > 0 ? R.frames / s : 0., R.slow, (double)R.longest / 1000.0);
    RaceLink L[NLINKS];
    int nl = vlan_race_links(L, NLINKS, 1);
    for (int i = 0; i < nl; i++){
        char a[16]; ip_str(L[i].ip_h, a);
        uint32_t runs = L[i].runs - L[i].gaps, rest = L[i].n - L[i].after;
        fprintf(stderr, "[racenet]   link %s: %u datagrams (%.1f/s), longest wait %u ms, %u waits of %u ms or more",
                a, L[i].n, s > 0 ? L[i].n / s : 0., L[i].gap_max_ms, L[i].gaps, RACENET_GAP_MS);
        if (L[i].gaps) fprintf(stderr, ", %.1f at once after them", (double)L[i].after / L[i].gaps);
        fprintf(stderr, " (%.1f at once otherwise)\n", runs ? (double)rest / runs : 0.);
    }
    if (!R.host)
        fprintf(stderr, "[racenet]   straight between the joiners' games: %u car states sent, %u came, %u refused; "
                "the creator's list %s (%u other joiners)\n", R.cs_sent, R.cs_came, R.cs_refused,
                !direct_on() ? "not used (NFSU2_RACE_DIRECT=0)" : g_pl.at ? "came" : "never came (an older version)", g_pl.n);
    for (int i = 0; i < NCARS; i++){
        Car *e = &g_car[i];
        if (!e->car) continue;
        double span = e->n > 1 ? (double)(e->last - e->first) / 1e6 : 0.;
        fprintf(stderr, "[racenet]   car %s: %u updates (%.1f/s, %u straight from their game), longest wait %.0f ms, "
                "waits over 100/250/500 ms %u/%u/%u, jumps over 0.5 m %u, over 2 m %u (largest %.1f m), "
                "as the game had them %u/%u (largest %.1f m), corrections faded %u, shown %u, far \"hits\" passed %u, "
                "its states %.0f ms old by our race clock (%.0f to %.0f)",
                e->name[0] ? e->name : "-", e->n, span > 0 ? (e->n - 1) / span : 0., e->direct, (double)e->gap_max / 1000.0,
                e->g100, e->g250, e->g500, e->shown.jumps, e->shown.big, (double)e->shown.max,
                e->game.jumps, e->game.big, (double)e->game.max, e->faded, e->kept, e->unhit,
                e->n ? e->age_sum / e->n * 1000.0 : 0., (double)e->age_min * 1000.0, (double)e->age_max * 1000.0);
        Fps *f = 0;
        for (int j = 0; j < NCARS && e->name[0]; j++) if (!strcmp(g_fps[j].name, e->name)) f = &g_fps[j];
        if (f && f->n) fprintf(stderr, "; their game %.1f fps (lowest %.1f), %u frames over 50 ms (longest %u ms), "
                               "their race clock %+.0f ms off ours\n", (double)(f->sum / (float)f->n), (double)f->lo,
                               f->slow, f->longest_ms, f->clock / f->n);
        else fprintf(stderr, "; their frame rate unknown (an older version)\n");
    }
    R.on = 0;
}

/* a car's place this frame into its jumps (racenet_frame) */
static void jumps_add(Jumps *j, const float *p, uint64_t dt, float step){
    float d[3];
    for (int i = 0; i < 3; i++) d[i] = p[i] - j->p[i];
    if (j->have > 1 && dt && j->step > 0.f){
        float s = step / j->step, x2 = 0;
        for (int i = 0; i < 3; i++) x2 += (d[i] - j->d[i] * s) * (d[i] - j->d[i] * s);
        float x = __builtin_sqrtf(x2);
        int jump = __builtin_isfinite(x) && x > 0.5f;
        if (jump && !j->jumped){ j->jumps++; j->big += x > 2.f; }
        if (jump && x > j->max) j->max = x;
        j->jumped = jump;
    }
    if (j->have) memcpy(j->d, d, sizeof d);
    memcpy(j->p, p, sizeof j->p); j->step = step; j->have += j->have < 2;
}

/* every frame, on the game thread (roam.c's main-loop hook) */
void racenet_frame(CPU *c){
    (void)c;
    uint32_t pl = rd32(G_PLAYER1), me = pl ? rd32(pl + 4u) : 0, w = rd32(G_WORLD), rc = rd32(G_RACE);
    int racing = rd32(G_FLOW) == 6u && (rd32(G_NETMODE) & 0x208u) && ok(me, 0x9e0) && ok(w, 0x80) && rd32(rc) == RC_RACING &&
                 !finished(rc, me);
    uint64_t t = now_us();
    if (racing && !R.on) begin(t, me);
    else if (!racing && R.on) end(t);
    /* the creator's list of the joiners, the cars drawn (also once our own
     * player is through: the others still race) */
    if (direct_on() && is_host() && in_race() && ms_now() - g_pl_sent >= PL_MS){ g_pl_sent = ms_now(); list_send(); }
    fade_frame();
    if (!R.on) return;
    /* our frames (the first one has no length) */
    uint64_t dt = R.prev ? t - R.prev : 0;
    if (R.prev){
        R.frames++; R.wframes++;
        if (dt > SLOW_US){ R.slow++; R.wslow++; }
        if (dt > R.longest) R.longest = dt;
        if (dt > R.wlongest) R.wlongest = dt;
    }
    R.prev = t;
    /* the other cars as shown (the fade's offset on) and as the game had them
     * (without): a jump is a frame's move that differs from the frame
     * before's, scaled to this frame's length, by over 0.5 m (a car that
     * stands still and then steps, or snaps: a hit's change of speed moves it
     * a few cm a frame).  Not the car's speed: a far car's reads 0.  The
     * frame's length is the world's step (a world that catches up after a
     * stall moves every car that far), the wall clock's only when the world
     * stood.  (It was the longer of the two: on a busy machine the frame
     * pacer gives steps of 16.7, 33.3 and 50 ms that take other wall times,
     * the cars move by the steps, and 20-50 jumps a minute of every car,
     * the creator's too, were that, NFSU2_RACENET_TRACE showed.)  Two frames
     * in a row count once. */
    float wdt = MRF32(G_WORLDDT), fdt = (float)dt / 1e6f, step = wdt > 0.f && wdt < 1.f ? wdt : fdt;
    static int trace = -1;                           /* NFSU2_RACENET_TRACE=1: every frame of every car (tests) */
    if (trace < 0){ const char *e = getenv("NFSU2_RACENET_TRACE"); trace = e && e[0] == '1'; }
    static uint32_t clk_prev; uint32_t clk = rd32(G_WORLDTIME);
    float drt = clk_prev ? (float)(int32_t)(clk - clk_prev) * 0.00025f : 0.f;
    clk_prev = clk;
    uint32_t car = rd32(w);
    for (int k = 0; car && car != w && k < 64 && ok(car, 0x9e0); k++, car = rd32(car)){
        Car *e = car == me ? 0 : car_of(car, 0);
        if (!e) continue;
        float p[3], g[3];
        rd3(car + CAR_POS, p);
        memcpy(g, p, sizeof g);
        Fade *f = fade_of(car, 0);
        if (f && f->put) for (uint32_t i = 0; i < 3; i++){ union { uint32_t u; float x; } b = { f->was60[i] }; g[i] = b.x; }
        uint32_t js = e->shown.jumps, jg = e->game.jumps;
        float sp[3], gp[3];
        memcpy(sp, e->shown.p, sizeof sp); memcpy(gp, e->game.p, sizeof gp);
        jumps_add(&e->shown, p, dt, step);
        jumps_add(&e->game, g, dt, step);
        if (trace){
            float ds = 0, dg = 0, o = 0;
            for (int i = 0; i < 3; i++){ ds += (p[i] - sp[i]) * (p[i] - sp[i]); dg += (g[i] - gp[i]) * (g[i] - gp[i]); o += (p[i] - g[i]) * (p[i] - g[i]); }
            fprintf(stderr, "[rntrace] %s wall %.1f world %.1f clock %.1f ms: moved %.3f m shown, %.3f m the game's, offset %.3f m%s%s\n",
                    e->name, (double)fdt * 1e3, (double)wdt * 1e3, (double)drt * 1e3, (double)__builtin_sqrtf(ds),
                    (double)__builtin_sqrtf(dg), (double)__builtin_sqrtf(o), e->shown.jumps != js ? " JUMP shown" : "",
                    e->game.jumps != jg ? " JUMP game" : "");
        }
    }
    /* our frame rate to the others */
    if (t - R.w0 >= FS_MS * 1000u && R.me[0]){
        uint8_t m[FS_N];
        uint32_t fps10 = (uint32_t)((double)R.wframes * 1e7 / (double)(t - R.w0) + 0.5), ms = (uint32_t)(R.wlongest / 1000u);
        if (fps10 > 0xffffu) fps10 = 0xffffu;
        if (ms > 0xffffu) ms = 0xffffu;
        memset(m, 0, sizeof m);
        memcpy(m, "N2FS", 4); m[4] = 1;
        m[6] = (uint8_t)fps10; m[7] = (uint8_t)(fps10 >> 8); m[8] = (uint8_t)ms; m[9] = (uint8_t)(ms >> 8);
        m[10] = (uint8_t)(R.wslow > 255u ? 255u : R.wslow);
        uint32_t clk = rd32(G_WORLDTIME);
        m[12] = (uint8_t)clk; m[13] = (uint8_t)(clk >> 8); m[14] = (uint8_t)(clk >> 16); m[15] = (uint8_t)(clk >> 24);
        memcpy(m + 16, R.me, strlen(R.me));
        send_all(m, 0);
        R.w0 = t; R.wframes = 0; R.wslow = 0; R.wlongest = 0;
    }
}
