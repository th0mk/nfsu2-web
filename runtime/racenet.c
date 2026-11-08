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
 *   - every car state carries everything, RATE_HZ a second: the game sent
 *     the place and orientation 20 times a second but the velocity and spin
 *     10 (and two more field groups 5: its scheduler's table 0x7FBE68 is per
 *     field group, not per distance), so every other state was carried on
 *     with a velocity up to 0.1 s old -- or the car's own here, which the
 *     controller takes after a change of 1 m/s (0x5F8D40);
 *   - the race clock (a joiner's follows the creator's): the game set it to
 *     single samples (the creator's clock plus half one round trip), one
 *     0.25 s off at once and smaller ones after 5 in a row on one side, and
 *     ran the whole world (our car too) 0.85-1.2 times as fast to get there
 *     (0x601A60).  Every car is shown by its states' age on that clock: 20 ms
 *     off is 1.2 m at 60 m/s.  Now the estimate takes only the answers with
 *     the least round trips (clock_sample), and WorldTimer alone goes to it
 *     (clock_frame): the world's step stays the frame's;
 *   - the cars where their owners have them now: the game showed a car
 *     blended from up to 10 predictions, each living the link's latency +
 *     0.43 s, weighted by what is left of its life (0x5F8AF0), so states
 *     0.5 s old still pulled it; and one that had none left (no state for
 *     that long) it braked to a stop at 0.7 and then put where the states
 *     said.  Now the car is put where its newest state carries it to (along
 *     the curve its path turns on, as free roam's solid cars) at the end of
 *     the step, a correction faded as free roam's are; after a hit of this
 *     game it runs on its own physics until a state from after the hit
 *     comes (as the game showed hits), and with no state for STALL_S on its
 *     own too, with its owner's last controls, then glides back.
 * Since 2026-11-04 a game of this version sends its own car to every racer
 * itself, every frame, and draws the others' from those (racecars.c, called
 * from sub_005f8af0 below); the way above stays for the car of a racer whose
 * game is older (none of those come), and the fade for every car.
 * NFSU2_RACE_SMOOTH=0 turns all of it off (the game's own, for A/B tests);
 * NFSU2_RACE_DIRECT=0 only the states straight from their games,
 * NFSU2_RACE_FADE=0 only the fading, NFSU2_RACE_RATE=0 the game's rates,
 * NFSU2_RACE_CLOCK=0 the game's race clock, NFSU2_RACE_PREDICT=0 the game's
 * blend (NFSU2_RACE_STALL=MS: how long a car is carried on; with the game's
 * blend only that a stalled car keeps its controls; 0 the game's brake).
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
 *   "N2PL", version 1, flags (PL_SLOTS: the slots follow), the count (u8),
 *   0, then that many addresses (u32, host order), then (PL_SLOTS) the slot
 *   each of them drives (a byte each, 0xFF none) and the creator's own: the
 *   creator's list of the other joiners, to each joiner every PL_MS.  Taken
 *   only from the race's creator, and car states only from the addresses on
 *   its list, only for a slot that is neither the creator's nor ours.  (A
 *   game without the slots reads the addresses and leaves the rest.)
 * Who may speak for whom: the game's own race link is a handshaken CommUDP
 * link to the creator, but anyone on the virtual LAN who sends a datagram to
 * our UDP 3658 becomes a "race peer" (net_lan.c's list).  So the creator
 * lists only the race peers its race link sends to (its joiners: an open
 * link drops datagrams from any other address and answers none of them,
 * 0x753211), a joiner takes the list only from an address its race link
 * sends to (vlan_race_sent_to: a joiner's game sends only to its creator),
 * and only from the first one it came from in this race; and each address
 * speaks for the slot its own game drives (racenet_slot_of): the creator
 * knows it from its race connections (each one's link to its joiner's
 * address, G_CONNS, and the joiner it is, G_SEATS) and lists it.  With a
 * list from an older creator (no slots) each address speaks for the first
 * slot its car states named, which no other address may name in this race
 * (until the 2026-11-05 security audit the only rule: a joiner that named
 * another's slot first, from the race's start, spoke for that car all race,
 * and every game drew it where that joiner said and reported its jumps
 * against the player who drove it).  Both reset when the race is over.
 * Games with and without these checks race together as before.  (The game's
 * own relay could already carry a joiner's message naming another slot: the
 * creator decodes a joiner's car by the connection's slot but passes the
 * message's bytes on as they are, and the other joiners read the slot from
 * them.)
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
#include "guest.h"
#include "game.h"
#include "racenet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define G_PLAYER1    SPLIT_PLAYERS_INDEX   /* Player*: +4 car */
#define RC_RACING    4u            /*   (a network race stays at 4 behind the results, until it is left) */
#define RCR_DONE     0x10u         /* a racer: +0 its car, +0x10 u16 how it finished, 0 not yet */
#define NP_NAME      0x12u
#define CTL_CAR      0x10u         /* a remote car's controller: its world car */
#define CTL_PREV_V   0x140u        /*   the car's velocity as the last blend left it (3 floats) */
#define CTL_HIT      0x184u        /*   world time (1/4000 s) of the last hit it saw */
#define ST_TIME      0x7cu         /* a car state: float, the sender's race clock (s) */
#define CAR_POS      0x60u         /* a car: position, velocity (3 floats each) */
#define CAR_VEL      0x70u
#define HIT_M        80.f          /* m: a remote car farther from the player's is out of reach of a hit here */
#define HORN_PORT    3659u         /* racehorn.c's */
#define PEER_MS      5000u
#define FS_MS        2000u
#define FS_N         (16 + NAME_N)
#define NAME_N       16
#define NCARS        8
/* a car's jump right after a wait of over STALL_US for its states (within STALL_AFTER_US of the state that ended it)
 * is what a stall does, up to what a car drives meanwhile (STALL_MPS a second of the wait): the race's report
 * (racenet_jump) counts only the rest of it (a player who holds its states back, then teleports: still a jump) */
#define STALL_US       1000000u
#define STALL_AFTER_US 500000u
#define STALL_MPS      100.f
#define NLINKS       16
#define SLOW_US      50000u        /* a frame longer than this is a stall */
/* the car states (above) */
#define G_MYSLOT     0x007fbe5cu   /* a joiner's slot, and the creator's (the creator's welcome, 0x58BFE0) */
#define G_HOSTSLOT   0x007fbe64u
#define G_SLOTBITS   0x008691b4u   /* a slot's bits in a message, and the least slot (0x583F80) */
#define G_SLOTMIN    0x008691b8u
#define FN_JOIN_MSG  0x0058e900u   /* a joiner's handler of the creator's messages (cdecl: the bytes, the length, 0) */
#define MSG_CARSPAM  2u
#define BS_BYTES     0x400u        /* a bit stream: the bytes, then +0x400 the bits written */
#define CS_HDR       8u
#define PL_MS        1000u         /* the creator's list of the joiners, every PL_MS */
#define PL_STALE_MS  5000u         /*   and the list a joiner goes by, at most this old */
#define NPL          8
#define PL_SLOTS     1u            /*   its flag: the slots follow the addresses */
/* who drives which slot (above): the creator's race connections (0x5877D0 runs them) */
#define G_CONNS      0x008691d0u   /* CONN_N of CONN_SZ: +0 state (CONN_UP), +4 its id, +0xC its link (+0 its CommUDP port) */
#define CONN_N       5u
#define CONN_SZ      0x14u
#define CONN_UP      3u
#define PORT_PEER    0x7cu         /* a CommUDP port: its peer's sockaddr (family, port, address in network order) */
#define G_SEATS      0x008693c8u   /* the creator's joiners (0x5895C0): SEAT_N of SEAT_SZ, +0 the slot, +4 the connection's id */
#define SEAT_N       5u
#define SEAT_SZ      0x20u
#define SLOT_ANY     (-2)          /* racenet_slot_of: not known (an older creator's list), the first slot named */
/* the fade (above) */
#define FADE_MIN     0.15f         /* m: a frame's move this much off the car's own (its velocity, its last move) is a correction */
#define FADE_MAX     10.f          /* m: a correction (with what still shows of the last ones) over this is shown */
#define FADE_W       20.f          /* 1/s: the fade, critically damped: 9 % left after 0.2 s */
/* the car states' rates (above): the scheduler's table, a field group's states a second */
#define G_RATES      0x007fbe68u   /* 4 dwords: flags 1 (place, orientation, ...), 2 (velocity, spin), 4, 8 */
#define RATE_HZ      30u           /*   ours: every state carries everything, this often */
/* the race clock (above) */
#define G_CLK_TARGET 0x00869234u   /* the clock the game steers WorldTimer to (1/4000 s) */
#define G_CLK_FLAG   0x00866758u   /*   (byte: steer to it) */
#define G_CLK_ECHO   0x007fbe60u   /* float: the newest echo of our real-time clock the creator answered */
#define G_WORLDTIME_S 0x007fb718u  /* float: WorldTimer in seconds (0x57A0E0 keeps it with each step) */
#define BS_READ      0x404u        /* a bit stream: the bits read */
#define CK_N         128           /* clock samples kept */
#define CK_SPAN      3.0           /* s: the least round trip over this */
#define CK_FRESH     1.0           /* s: the estimate from samples this new */
#define CK_TAU       0.15          /* s: WorldTimer goes to the estimate with this time constant */
#define CK_TAU_I     1.0           /* s: and learns how fast it drifts over this */
#define CK_ILIM      0.02          /* s: (off by less than this) */
#define CK_HARD      0.25          /* s: an error over this is set at once (as the game) */
#define CK_DRIFT     0.15          /* s/s: the most the creator's clock drifts from our real time */
#define CK_STEP      0.015         /* s: a step of the creator's clock (above) */
/* the cars where their owners have them now (above) */
#define ST_POS       0x00u         /* a car state: place, velocity, (acceleration), spin, (+0x40), orientation (x, y, z, w) */
#define ST_VEL       0x10u
#define ST_SPIN      0x30u
#define ST_QUAT      0x50u
#define ST_BRAKE     0x70u         /*   the controls: the brake, the throttle, +0x78, u16 +0x94 (0x5F8900 puts them in the car) */
#define ST_GAS       0x74u
#define ST_X78       0x78u
#define ST_W94       0x94u
#define CTL_RESET    0x150u        /* a remote car's controller: byte, drop every prediction (a reset) */
#define CTL_NPRED    0x0cu         /*   its predictions */
#define CTL_BLEND    0xc0u         /*   the blend's state: +0x10 place, +0x20 velocity, +0x30 orientation, +0x40 spin */
#define CAR_INPUT    0x30u         /* a car's input: +0x20C the throttle, +0x210, +0x214 the brake, u16 +0x1E0 */
#define FN_APPLY     0x005f8810u   /* the blend's state into the car (thiscall: the state, the car) */
#define RET_APPLY    0x005f8cf7u   /*   (as the blend calls it) */
#define FN_DRIVEN    0x005a2a60u   /* a car of kind 3 driven by its states (the blend's) */
#define RET_DRIVEN   0x005f8d0cu
#define STALL_S      0.75f         /* s: a car is carried on this long past its newest state, then runs on its own */
#define SNAP_M       25.f          /* m: a correction over this is shown at once */
#define GLIDE_V      25.f          /* m/s: a correction after a car ran on its own glides about this fast ... */
#define GLIDE_TAU_MIN 0.08f        /* s: ... in a time constant between these */
#define CORR_TAU     0.12f         /* s: a correction from new states fades with this time constant (at least) */
#define GLIDE_TAU_MAX 0.35f
#define TURN_MAX     3.f           /* rad/s: the most a carried-on car's course turns */

/* net_lan.c in the browser (racehorn.c has the stand-ins for the others) */
int vlan_rt_send(uint16_t port, uint32_t ip_h, const void *p, int n);
int vlan_race_peers(uint32_t *ip_h, int max, uint32_t within_ms);
int vlan_race_sent_to(uint32_t ip_h, uint32_t within_ms);
__attribute__((weak)) int vlan_race_links(RaceLink *o, int max, int reset){ (void)o; (void)max; (void)reset; return 0; }
/* racecars.c: the other cars as free roam sends and shows them, where their games send them */
int  racecars_pre(uint32_t car);
int  racecars_step(CPU *c, uint32_t ctl, uint32_t car);
void racecars_frame(CPU *c);
void racecars_log(double secs);

static void rd3(uint32_t a, float *o){ o[0] = MRF32(a); o[1] = MRF32(a + 4u); o[2] = MRF32(a + 8u); }
static uint64_t now_us(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
/* the name of the player whose car this is, as racehorn.c's car_name */
static void car_name(uint32_t car, char *o){
    int i = guest_ok(car, 0x9e0) ? (int16_t)MR16(car + 0xau) : -1;
    uint32_t pl = i >= 0 && i < 6 ? guest_rd32(G_NETPLAYERS + 4u * (uint32_t)i) : 0;
    int n = 0;
    if (guest_ok(pl, NP_NAME + NAME_N))
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
    uint32_t pl_refused;                             /* lists not from the race's creator */
} R;
/* a car's jumps: a frame whose move differs from the frame before's */
typedef struct { float p[3], d[3], step; int have, jumped; uint32_t jumps, big; float max; } Jumps;
/* the other cars, by their world car */
typedef struct {
    uint32_t car; char name[NAME_N];
    uint32_t n, direct, g100, g250, g500; uint64_t first, last, gap_max;
    Jumps shown, game;                                /* as drawn, and as the game had it */
    uint64_t gap_last, gap_at;                        /* the wait before its last state, and when that came */
    float jump;                                       /* its largest jump as the game had it, not after a stall
                                                       * (racenet_jump: the race's report to the server) */
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
/* Test knobs (A/B on the rig), read only by a game running a test script
 * (NFSU2_INPUT_SCRIPT: settings.ini and the page address can't set it), so a
 * player can't, say, send their own car seldom to be hard to hit:
 * NFSU2_RACE_RATE=N the car states' rate (RATE_HZ, every field each time; 0
 * the game's 20/10/5/5); NFSU2_RACE_CLOCK=0 the game's race clock;
 * NFSU2_RACE_PREDICT=0 the game's blend of predictions; NFSU2_RACE_STALL=MS
 * how long a car is carried on past its newest state (0: the game's brake) */
static const char *test_env(const char *k){
    const char *s = getenv("NFSU2_INPUT_SCRIPT");
    return s && *s ? getenv(k) : 0;
}
static int test_on(const char *k){ const char *e = test_env(k); return !(e && e[0] == '0'); }
static int rate_hz(void){
    static int hz = -2;
    if (hz == -2){
        const char *e = test_env("NFSU2_RACE_RATE");
        hz = !smooth_on() ? 0 : e && *e ? atoi(e) : (int)RATE_HZ;
        if (hz < 0 || hz > 120) hz = (int)RATE_HZ;
    }
    return hz;
}
static int clock_on(void){
    static int on = -1;
    if (on < 0) on = smooth_on() && test_on("NFSU2_RACE_CLOCK");
    return on;
}
static int predict_on(void){
    static int on = -1;
    if (on < 0) on = smooth_on() && test_on("NFSU2_RACE_PREDICT");
    return on;
}
/* test knobs for the A/B: NFSU2_RACE_TAU=MS the correction's fade (time
 * constant), NFSU2_RACE_LEAD=MS how far past the step's start (WorldTimer
 * during it) the cars are put: the rig had the game's own place (the step's
 * start) right on average, and its end a step ahead */
static float tau_s(void){
    static float s = -1.f;
    if (s < 0.f){ const char *e = test_env("NFSU2_RACE_TAU"); s = e && *e ? (float)atoi(e) / 1000.f : CORR_TAU; if (!(s >= 0.01f && s <= 2.f)) s = CORR_TAU; }
    return s;
}
static float lead_s(void){
    static float s = -1e9f;
    if (s < -1e8f){ const char *e = test_env("NFSU2_RACE_LEAD"); s = e && *e ? (float)atoi(e) / 1000.f : 0.f; if (!(s >= -0.5f && s <= 0.5f)) s = 0.f; }
    return s;
}
static float stall_s(void){
    static float s = -1.f;
    if (s < 0.f){
        const char *e = test_env("NFSU2_RACE_STALL");
        s = !smooth_on() ? 0.f : e && *e ? (float)atoi(e) / 1000.f : STALL_S;
        if (!(s >= 0.f && s <= 5.f)) s = STALL_S;
    }
    return s;
}
/* a network race runs here (also after our own player finished: the
 * others still race, and still come) */
static int in_race(void){
    uint32_t w = guest_rd32(G_WORLD), rc = guest_rd32(G_RACE);
    return guest_rd32(G_FLOW) == 6u && (guest_rd32(G_GAMEMODE) & 0x208u) && guest_ok(w, 0x80) && guest_rd32(rc) == RC_RACING;
}
static int is_host(void){ return (int32_t)guest_rd32(G_NET_HOST) > 0; }
static uint32_t ms_now(void){ return (uint32_t)(now_us() / 1000u); }
/* the car is farther than HIT_M from the player's */
static int far_from_me(uint32_t car){
    uint32_t pl = guest_rd32(G_PLAYER1), me = pl ? guest_rd32(pl + 4u) : 0;
    if (!guest_ok(me, CAR_POS + 12u) || !guest_ok(car, CAR_VEL + 12u) || car == me) return 0;
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
    uint32_t ctl = c->ecx, car = guest_ok(ctl, CTL_HIT + 4u) ? guest_rd32(ctl + CTL_CAR) : 0;
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

/* ---- the cars where their owners have them now: their states (above) ---- */
typedef struct { float t, p[3], v[3], w[3], q[4]; } PState;
typedef struct {
    uint32_t car;
    int n;                                    /* states kept: [1] the newest, [0] the one before */
    PState s[2];
    float gas, brake, x78; uint16_t w94;      /* the newest state's controls */
    int shown, own;                           /* put last step; the car ran on its own (a hit, a stall) */
    float tp[3], tq[4], tt;                   /* last step's target (without the correction) and its race time */
    float ep[3], eq[4], tau;                  /* the correction still shown, and how fast it goes */
    uint32_t steps, own_steps, glides, snaps, old_states; /* for the log */
    float glide_max, carried_max;
} Pred;
static Pred g_pred[NCARS];
static uint32_t g_pred_world;
/* a car's (add: a new world forgets the last one's cars) */
static Pred *pred_of(uint32_t car, int add){
    uint32_t w = guest_rd32(G_WORLD);
    if (add && w != g_pred_world){ memset(g_pred, 0, sizeof g_pred); g_pred_world = w; }
    if (w != g_pred_world) return 0;
    for (int i = 0; i < NCARS; i++) if (g_pred[i].car == car) return &g_pred[i];
    if (!add) return 0;
    for (int i = 0; i < NCARS; i++) if (!g_pred[i].car){ memset(&g_pred[i], 0, sizeof g_pred[i]); g_pred[i].car = car; g_pred[i].eq[3] = 1.f; return &g_pred[i]; }
    return 0;
}
static float len3(const float *a);
static void pq_mul(const float *a, const float *b, float *o);
static void pq_conj(const float *a, float *o);
static int finite_n(const float *a, int n){ for (int i = 0; i < n; i++) if (!__builtin_isfinite(a[i])) return 0; return 1; }
static void pq_norm(float *q){
    float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (!(n > 1e-12f) || !__builtin_isfinite(n)){ q[0] = q[1] = q[2] = 0.f; q[3] = 1.f; return; }
    float k = 1.f / __builtin_sqrtf(n);
    for (int i = 0; i < 4; i++) q[i] *= k;
}
/* a state the game took for a remote car (0x5F8900's): kept when newer.
 * With every field in every state (RATE_HZ) they are its owner's own; an
 * older version's states keep what the controller held for the fields not
 * sent, as before */
static void pred_state(uint32_t car, uint32_t st){
    if (!guest_ok(st, 0x98u)) return;
    PState s;
    s.t = MRF32(st + ST_TIME);
    rd3(st + ST_POS, s.p); rd3(st + ST_VEL, s.v); rd3(st + ST_SPIN, s.w);
    for (uint32_t k = 0; k < 4; k++) s.q[k] = MRF32(st + ST_QUAT + 4u * k);
    if (!finite_n(&s.t, 1) || !finite_n(s.p, 3) || !finite_n(s.v, 3) || !finite_n(s.w, 3) || !finite_n(s.q, 4)) return;
    pq_norm(s.q);
    Pred *p = pred_of(car, 1);
    if (!p || (p->n && !(s.t > p->s[1].t))) return;
    /* An older game sends the velocity and spin with every other state only:
     * the others keep the last ones bit for bit (the controller's).  A moving
     * car's own never repeat, so a repeated pair is old: its velocity from the
     * two states' places, its spin from their orientations */
    float dt = p->n ? s.t - p->s[1].t : 0.f;
    if (p->n && dt >= 0.005f && dt <= 0.25f && !memcmp(s.v, p->s[1].v, sizeof s.v) && len3(s.v) > 1.f){
        float dq[4], ic[4];
        for (int i = 0; i < 3; i++) s.v[i] = (s.p[i] - p->s[1].p[i]) / dt;
        if (!memcmp(s.w, p->s[1].w, sizeof s.w)){
            pq_conj(p->s[1].q, ic); pq_mul(s.q, ic, dq);
            float sg = dq[3] < 0.f ? -2.f : 2.f;
            for (int i = 0; i < 3; i++) s.w[i] = sg * dq[i] / dt;
        }
        p->old_states++;
    }
    if (p->n) p->s[0] = p->s[1];
    p->s[1] = s; p->n += p->n < 2;
    p->gas = MRF32(st + ST_GAS); p->brake = MRF32(st + ST_BRAKE); p->x78 = MRF32(st + ST_X78); p->w94 = MR16(st + ST_W94);
}

/* a car state came for a remote car (thiscall: the controller, the state) */
void sub_005f8900_orig(CPU *c);
void sub_005f8900(CPU *c){
    uint32_t ctl = c->ecx, st = MR32(c->esp + 4u), car = guest_ok(ctl, CTL_CAR + 4u) ? MR32(ctl + CTL_CAR) : 0;
    Car *e = car && R.on ? car_of(car, 1) : 0;
    if (car && g_fade_world && guest_rd32(G_WORLD) == g_fade_world) fade_of(car, 1);
    if (e){
        e->direct += g_direct;
        uint64_t t = now_us();
        if (e->n){
            uint64_t g = t - e->last;
            if (g > e->gap_max) e->gap_max = g;
            e->gap_last = g; e->gap_at = t;
            e->g100 += g > 100000u; e->g250 += g > 250000u; e->g500 += g > 500000u;
        } else e->first = t;
        if (guest_ok(st, ST_TIME + 4u)){                        /* the state's age by this game's race clock */
            float age = (float)guest_rd32(G_WORLDTIMER) * 0.00025f - MRF32(st + ST_TIME);
            if (__builtin_isfinite(age)){
                if (!e->n || age < e->age_min) e->age_min = age;
                if (!e->n || age > e->age_max) e->age_max = age;
                e->age_sum += age;
            }
        }
        e->n++; e->last = t;
        if (!e->name[0]) car_name(car, e->name);
    }
    if (car && (predict_on() || stall_s() > 0.f) && in_race()) pred_state(car, st);
    sub_005f8900_orig(c);
}

/* ---- car states straight from their games (above) ---- */
static struct { uint32_t from, at, n, ip[NPL]; int slots, host; int8_t slot[NPL]; } g_pl;   /* a joiner: the creator's list
                                                        * (host order), when it came; with the slots: each one's, the creator's */
static struct { uint32_t ip, slot; } g_bind[NPL]; static int g_nbind;   /* this race: the slot each joiner's states name */
/* out of a network race: no list, no slots (the next race starts anew) */
static void direct_reset(void){ memset(&g_pl, 0, sizeof g_pl); g_nbind = 0; }
static uint32_t g_pl_sent;                              /* the creator: when it last sent it */

/* A message goes to the creator (cdecl: its kind for the game's statistics,
 * the bit stream, 0): a joiner's car state (0x587830's) also goes straight
 * to the other joiners on the creator's list. */
void sub_00585d50_orig(CPU *c);
static void clock_sent(uint32_t bs);
void sub_00585d50(CPU *c){
    uint32_t kind = MR32(c->esp + 4u), bs = MR32(c->esp + 8u);
    if (kind == MSG_CARSPAM) clock_sent(bs);
    if (kind == MSG_CARSPAM && direct_on() && g_pl.n && ms_now() - g_pl.at <= PL_STALE_MS && !is_host() && in_race() &&
        guest_ok(bs, BS_BYTES + 4u)){
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
/* the creator: the slot its race connection to ip (host order) drives, -1
 * none (the connection whose link's peer is that address, the joiner that
 * connection is) */
static int conn_slot(uint32_t ip){
    for (uint32_t i = 0; i < CONN_N; i++){
        uint32_t e = G_CONNS + CONN_SZ * i, port = guest_rd32(guest_rd32(e + 0xcu));
        if (guest_rd32(e) != CONN_UP || !guest_ok(port, PORT_PEER + 8u) || MR16(port + PORT_PEER) != 2u ||
            __builtin_bswap32(MR32(port + PORT_PEER + 4u)) != ip) continue;
        uint32_t id = guest_rd32(e + 4u);
        for (uint32_t k = 0; k < SEAT_N; k++)
            if (guest_rd32(G_SEATS + SEAT_SZ * k + 4u) == id){
                int32_t s = (int32_t)guest_rd32(G_SEATS + SEAT_SZ * k);
                return s >= 0 && s < 6 ? s : -1;
            }
    }
    return -1;
}
/* the slot the racer at ip (host order) drives (racecars.c's N2RS, our
 * N2CS): on the creator its race connection's, on a joiner the creator's
 * list's (the creator's own for the address the list came from); -1 none,
 * SLOT_ANY not known (no list yet, or an older creator's without the slots:
 * the first slot it names, above) */
int racenet_slot_of(uint32_t ip){
    if (is_host()) return conn_slot(ip);
    if (!g_pl.slots) return SLOT_ANY;
    if (ip == g_pl.from) return g_pl.host;
    for (uint32_t k = 0; k < g_pl.n; k++) if (g_pl.ip[k] == ip) return g_pl.slot[k];
    return -1;
}
/* the creator's list to each joiner: the others and their slots (every
 * PL_MS, racenet_frame).  Only the race peers the game's race link sends to
 * (its joiners), not everyone who sent a datagram to our UDP 3658 (above). */
static void list_send(void){
    uint32_t all[16], peers[NPL], pl = guest_rd32(G_PLAYER1), me = pl ? guest_rd32(pl + 4u) : 0;
    int na = vlan_race_peers(all, 16, PEER_MS), n = 0, slot[NPL];
    int16_t own = guest_ok(me, 0x9e0) ? (int16_t)MR16(me + 0xau) : -1;
    for (int i = 0; i < na && n < NPL; i++) if (vlan_race_sent_to(all[i], PEER_MS)) peers[n++] = all[i];
    for (int i = 0; i < n; i++) slot[i] = conn_slot(peers[i]);
    for (int i = 0; i < n; i++){
        uint8_t m[8 + 5 * NPL + 1];
        int k = 0;
        memcpy(m, "N2PL", 4); m[4] = 1; m[5] = PL_SLOTS; m[7] = 0;
        for (int j = 0; j < n; j++){
            if (j == i) continue;
            for (int b = 0; b < 4; b++) m[8 + 4 * k + b] = (uint8_t)(peers[j] >> (8 * b));
            k++;
        }
        m[6] = (uint8_t)k;
        int p = 8 + 4 * k;
        for (int j = 0; j < n; j++) if (j != i) m[p++] = slot[j] >= 0 ? (uint8_t)slot[j] : 0xffu;
        m[p++] = own >= 0 && own < 6 ? (uint8_t)own : 0xffu;
        vlan_rt_send(HORN_PORT, peers[i], m, p);
    }
}
/* the other racers' addresses (racecars.c sends them our car): the creator's
 * joiners (the race peers its race link sends to), a joiner's creator (the
 * one its race link sends to) and the others on the creator's list */
int racenet_race_peers(uint32_t *ip, int max){
    uint32_t all[16];
    int na = vlan_race_peers(all, 16, PEER_MS), n = 0;
    for (int i = 0; i < na && n < max; i++) if (vlan_race_sent_to(all[i], PEER_MS)) ip[n++] = all[i];
    if (!is_host() && g_pl.n && ms_now() - g_pl.at <= PL_STALE_MS)
        for (uint32_t k = 0; k < g_pl.n && n < max; k++){
            int dup = 0;
            for (int j = 0; j < n; j++) dup |= ip[j] == g_pl.ip[k];
            if (!dup) ip[n++] = g_pl.ip[k];
        }
    return n;
}
/* an N2PL from the horn's port (racehorn.c): only from the race's creator,
 * an address the game's race link sends to, and the first one in this race
 * (above) */
void racenet_list(const uint8_t *m, int n, uint32_t from){
    if (n < 8 || m[4] != 1 || m[6] > NPL || n < 8 + 4 * m[6] || is_host()) return;
    if (!vlan_race_sent_to(from, PEER_MS) || (g_pl.from && g_pl.from != from)){ R.pl_refused++; return; }
    g_pl.from = from; g_pl.at = ms_now(); g_pl.n = 0;
    /* the slots (an older creator's list has none: SLOT_ANY, as before) */
    const uint8_t *sl = (m[5] & PL_SLOTS) && n >= 8 + 5 * m[6] + 1 ? m + 8 + 4 * m[6] : 0;
    g_pl.slots = sl != 0;
    g_pl.host = sl && sl[m[6]] < 6 ? sl[m[6]] : -1;
    for (int k = 0; k < m[6]; k++){
        const uint8_t *q = m + 8 + 4 * k;
        uint32_t a = (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
        if (!a || a == from) continue;
        g_pl.slot[g_pl.n] = (int8_t)(sl && sl[k] < 6 ? sl[k] : -1);
        g_pl.ip[g_pl.n++] = a;
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
 * the creator's (whose states the game answers), and only for the slot the
 * list says that address drives (an older creator's list: the slot it named
 * first in this race, if no other address named it before). */
void racenet_carspam(CPU *c, const uint8_t *m, int n, uint32_t from){
    if (!direct_on() || n < (int)CS_HDR || m[4] != 1 || is_host() || !in_race()) return;
    uint32_t len = (uint32_t)m[6] | (uint32_t)m[7] << 8, nb = guest_rd32(G_SLOTBITS), known = 0;
    for (uint32_t k = 0; k < g_pl.n; k++) known |= g_pl.ip[k] == from;
    if (!known || ms_now() - g_pl.at > PL_STALE_MS || len < 6u || len > BS_BYTES || (uint32_t)n < CS_HDR + len ||
        m[CS_HDR] != MSG_CARSPAM || nb < 1u || nb > 8u || 40u + nb > 8u * len){ R.cs_refused++; return; }
    uint32_t slot = bits_at(m + CS_HDR, 40u, nb) + guest_rd32(G_SLOTMIN);
    if (slot >= 6u || slot == guest_rd32(G_MYSLOT) || slot == guest_rd32(G_HOSTSLOT)){ R.cs_refused++; return; }
    /* the slot the creator's list says it drives */
    int want = racenet_slot_of(from);
    if (want != SLOT_ANY && (uint32_t)want != slot){ R.cs_refused++; return; }
    if (want == SLOT_ANY){
        /* an older creator's list: one slot an address, one address a slot, for this race (above) */
        int b = 0;
        while (b < g_nbind && g_bind[b].ip != from) b++;
        if (b < g_nbind && g_bind[b].slot != slot){ R.cs_refused++; return; }
        if (b == g_nbind){
            for (int i = 0; i < g_nbind; i++) if (g_bind[i].slot == slot){ R.cs_refused++; return; }
            if (g_nbind == NPL){ R.cs_refused++; return; }
            g_bind[g_nbind].ip = from; g_bind[g_nbind].slot = slot; g_nbind++;
        }
    }
    R.cs_came++;
    /* the bytes below the hook's frame, the call below them */
    CPU save = *c;
    uint32_t buf = (c->esp - 0x1000u - len) & ~15u;
    for (uint32_t i = 0; i < len; i++) MR8(buf + i) = m[CS_HDR + i];
    c->esp = buf - 64u;
    PUSH32(0); PUSH32(len); PUSH32(buf);
    PUSH32(GUEST_RET_MAINLOOP);
    g_direct = 1;
    dispatch_indirect(c, FN_JOIN_MSG);
    g_direct = 0;
    *c = save;
}

/* ---- the fade (above) ---- */
/* the car is on the world's list still */
static int in_world(uint32_t w, uint32_t car){
    uint32_t x = guest_rd32(w);
    for (int k = 0; x && x != w && k < 64 && guest_ok(x, 0x9e0); k++, x = guest_rd32(x)) if (x == car) return 1;
    return 0;
}
/* the cars' own places back: before the world's step, and before a frame
 * that had none puts its offsets on again.  A place the game changed since
 * (a reset) is the game's, the offset goes. */
static void fade_back(void){
    uint32_t w = guest_rd32(G_WORLD);
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
    uint32_t w = guest_rd32(G_WORLD);
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
    uint32_t pl = guest_rd32(G_PLAYER1), me = pl ? guest_rd32(pl + 4u) : 0;
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

/* ---- the cars where their owners have them now: each world step (above) ---- */
static void pq_mul(const float *a, const float *b, float *o){   /* o = a * b, (x, y, z, w) */
    float x = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    float y = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    float z = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    float w = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}
static void pq_conj(const float *a, float *o){ o[0] = -a[0]; o[1] = -a[1]; o[2] = -a[2]; o[3] = a[3]; }
static void pq_nlerp(const float *a, const float *b, float t, float *o){
    float d = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3], s = d < 0.f ? -1.f : 1.f;
    for (int i = 0; i < 4; i++) o[i] = (1.f - t) * a[i] + t * s * b[i];
    pq_norm(o);
}
/* q turned by the world angular velocity w over dt (4 steps, as roam_core.c's) */
static void pq_spin(const float *q0, const float *w, float dt, float *o){
    float q[4] = { q0[0], q0[1], q0[2], q0[3] }, h = dt * 0.25f;
    for (int k = 0; k < 4; k++){
        float wq[4] = { w[0], w[1], w[2], 0.f }, d[4];
        pq_mul(wq, q, d);
        for (int i = 0; i < 4; i++) q[i] += 0.5f * h * d[i];
        pq_norm(q);
    }
    for (int i = 0; i < 4; i++) o[i] = q[i];
}
static float clampf(float x, float lo, float hi){ return x < lo ? lo : x > hi ? hi : x; }
/* Where the car is at race time t by its newest state: carried on along the
 * curve its path turns on, as free roam's solid cars (roam_core.c
 * rc_extrap_arc): the velocity turned by the turn rate over the time, the
 * place its integral; the turn rate how much its velocity turned from the
 * state before (its path), else its yaw rate.  Up as the game carries a car
 * on (0x5EF470: a rising car's climb at 65 %); the orientation by its spin */
static void pred_at(const Pred *p, float t, float *op, float *ov, float *oq){
    const PState *s = &p->s[1];
    float ds = t - s->t;
    if (ds < 0.f) ds = 0.f;
    float k = s->w[2];
    if (p->n == 2){
        const PState *a = &p->s[0];
        float dt = s->t - a->t, sa = a->v[0] * a->v[0] + a->v[1] * a->v[1], sb = s->v[0] * s->v[0] + s->v[1] * s->v[1];
        if (dt >= 0.01f && dt <= 0.15f && sa > 9.f && sb > 9.f)
            k = __builtin_atan2f(a->v[0] * s->v[1] - a->v[1] * s->v[0], a->v[0] * s->v[0] + a->v[1] * s->v[1]) / dt;
    }
    k = clampf(k, -TURN_MAX, TURN_MAX);
    float a = clampf(k * ds, -1.f, 1.f), a2 = a * a;
    float sa = a * (1.f - a2 / 6.f * (1.f - a2 / 20.f)), ca = 1.f - a2 * (0.5f - a2 / 24.f * (1.f - a2 / 30.f));
    float c1 = ds * (1.f - a2 / 6.f * (1.f - a2 / 20.f)), c2 = ds * a * (0.5f - a2 / 24.f * (1.f - a2 / 30.f));
    float vx = s->v[0], vy = s->v[1];
    op[0] = s->p[0] + c1 * vx - c2 * vy;
    op[1] = s->p[1] + c2 * vx + c1 * vy;
    op[2] = s->p[2] + (s->v[2] > 0.f ? 0.65f : 1.f) * s->v[2] * ds;
    ov[0] = ca * vx - sa * vy; ov[1] = sa * vx + ca * vy; ov[2] = s->v[2];
    pq_spin(s->q, s->w, ds, oq);
}
/* a guest function (thiscall or cdecl: the whole CPU comes back) from a lifted
 * one's override, with the return address its own caller in the game uses */
static void call_game(CPU *c, uint32_t fn, uint32_t ecx, int n, const uint32_t *arg, uint32_t ret){
    guest_call_ex(c, fn, ecx, c->edx, n, arg, ret, 512u);
}
/* the newest state's controls, as 0x5F8900 put them (the game's blend
 * brakes a car with no prediction left: 0.7, nothing else) */
static void pred_controls(const Pred *p, uint32_t car){
    uint32_t in = guest_rd32(car + CAR_INPUT);
    if (!guest_ok(in, 0x218u)) return;
    MRF32(in + 0x20cu) = p->gas; MRF32(in + 0x214u) = p->brake; MRF32(in + 0x210u) = p->x78; MR16(in + 0x1e0u) = p->w94;
}
static float len3(const float *a){ return __builtin_sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
static void pred_zero(Pred *p){ memset(p->ep, 0, sizeof p->ep); p->eq[0] = p->eq[1] = p->eq[2] = 0.f; p->eq[3] = 1.f; p->tau = GLIDE_TAU_MIN; }

/* The game's blend of a remote car (thiscall: the controller, the step; once
 * a world step, after the car's own physics, before the collisions): first
 * the game's own (its side effects, its "hit" and reset handling), then
 * the car where its owner has it now (pred_at, at WorldTimer: the step's
 * start, where the game puts its own), through the game's own apply.  A correction (new
 * states, back from running on its own) fades as free roam's do; one that
 * comes after the car ran on its own glides at about GLIDE_V.  A car runs on
 * its own physics, with its owner's last controls, after a hit of this game
 * since its newest state (as the game shows one: the car reacts here at
 * once) and when no state came for STALL_S (the game braked it to a stop and
 * then put it where the states said: 20-30 m in players' logs). */
void sub_005f8af0_orig(CPU *c);
void sub_005f8af0(CPU *c){
    uint32_t ctl = c->ecx, car = guest_ok(ctl, 0x1a0u) ? guest_rd32(ctl + CTL_CAR) : 0;
    Pred *p = car && (predict_on() || stall_s() > 0.f) && in_race() ? pred_of(car, 0) : 0;
    int reset = p && MR8(ctl + CTL_RESET), fed = car && racecars_pre(car);
    sub_005f8af0_orig(c);
    /* its own game sends it (racecars.c): drawn there; this way starts anew if those stop */
    if (fed && racecars_step(c, ctl, car)){ if (p){ p->shown = 0; p->own = 0; } return; }
    if (!p || !p->n || !guest_ok(car, 0x9e0) || !guest_ok(ctl + CTL_BLEND, 0x50u)) return;
    float h = MRF32(G_WORLDDT);
    if (!(h > 0.f && h < 0.25f)) h = 0.f;
    float now = (float)guest_rd32(G_WORLDTIMER) * 0.00025f + lead_s(), age = now - p->s[1].t;
    uint32_t hit = guest_rd32(ctl + CTL_HIT);
    /* stalled: no state for STALL_S and none of the game's predictions left
     * (while one lives the game's blend puts the car on it: carried on here) */
    int stalled = stall_s() > 0.f && age > stall_s() && guest_rd32(ctl + CTL_NPRED) == 0;
    if (!predict_on()){                         /* the game's blend: only the brake goes */
        if (stalled) pred_controls(p, car);
        return;
    }
    if (reset){ p->shown = 0; p->own = 0; pred_zero(p); }
    if ((hit && (float)hit * 0.00025f > p->s[1].t) || stalled){
        pred_controls(p, car);
        if (p->shown) p->own = 1;
        p->own_steps++;
        return;
    }
    if (age > p->carried_max) p->carried_max = age;
    float tp[3], tv[3], tq[4];
    pred_at(p, now, tp, tv, tq);
    uint32_t b = ctl + CTL_BLEND;
    if (!p->shown) pred_zero(p);
    else if (p->own){
        /* from where the car is (the blend's state: the car as the game left it) */
        float cp[3], cq[4], ic[4];
        rd3(b + 0x10u, cp);
        for (uint32_t k = 0; k < 4; k++) cq[k] = MRF32(b + 0x30u + 4u * k);
        pq_norm(cq);
        for (int i = 0; i < 3; i++) p->ep[i] = cp[i] - tp[i];
        pq_conj(tq, ic); pq_mul(cq, ic, p->eq); pq_norm(p->eq);
        float e = len3(p->ep);
        if (!finite_n(p->ep, 3) || e > SNAP_M){ pred_zero(p); p->snaps++; }
        else { p->tau = clampf(e / GLIDE_V, GLIDE_TAU_MIN, GLIDE_TAU_MAX); p->glides++; if (e > p->glide_max) p->glide_max = e; }
    } else {
        /* what the states say now about last step's time, against the target then */
        float wp[3], wv[3], wq[4], ic[4], cq[4], t4[4];
        pred_at(p, p->tt, wp, wv, wq);
        for (int i = 0; i < 3; i++) p->ep[i] += p->tp[i] - wp[i];
        pq_conj(wq, ic); pq_mul(p->tq, ic, cq); pq_mul(cq, p->eq, t4); pq_norm(t4);
        memcpy(p->eq, t4, sizeof t4);
        if (!finite_n(p->ep, 3) || len3(p->ep) > SNAP_M){ pred_zero(p); p->snaps++; }
    }
    if (h > 0.f){
        static const float id[4] = { 0.f, 0.f, 0.f, 1.f };
        float k = __builtin_expf(-h / (p->tau > tau_s() ? p->tau : tau_s())), r[4];
        for (int i = 0; i < 3; i++) p->ep[i] *= k;
        pq_nlerp(id, p->eq, k, r); memcpy(p->eq, r, sizeof r);
        if (p->tau > tau_s() && len3(p->ep) < 0.05f) p->tau = tau_s();
    }
    float op[3], oq[4];
    for (int i = 0; i < 3; i++) op[i] = tp[i] + p->ep[i];
    pq_mul(p->eq, tq, oq); pq_norm(oq);
    /* into the blend's state and the car, as the blend puts it (0x5F8C98..0x5F8D07) */
    for (uint32_t k = 0; k < 3; k++){ MRF32(b + 0x10u + 4u * k) = op[k]; MRF32(b + 0x20u + 4u * k) = tv[k]; MRF32(b + 0x40u + 4u * k) = p->s[1].w[k]; }
    for (uint32_t k = 0; k < 4; k++) MRF32(b + 0x30u + 4u * k) = oq[k];
    uint32_t a = car;
    call_game(c, FN_APPLY, b, 1, &a, RET_APPLY);
    pred_controls(p, car);
    uint32_t drv = guest_rd32(car + 0x28u) == 3u ? guest_rd32(car + 0x34u) : 0, x = drv ? guest_rd32(drv + 0x24u) : 0;
    if (guest_ok(x, 0x139u)){ MR8(x + 0x138u) = 1; call_game(c, FN_DRIVEN, drv, 0, 0, RET_DRIVEN); }
    for (uint32_t k = 0; k < 3; k++) MRF32(ctl + CTL_PREV_V + 4u * k) = MRF32(car + CAR_VEL + 4u * k);
    memcpy(p->tp, tp, sizeof tp); memcpy(p->tq, tq, sizeof tq); p->tt = now;
    p->shown = 1; p->own = 0; p->steps++;
}

/* ---- the race clock (above) ---- */
static struct {
    struct { double r, rtt, th; } s[CK_N]; int h, n;
    double th, drift; int have;                /* the creator's race clock minus our real-time clock (s) at at, and its drift (s/s) */
    double at, since, frac, prev_r;            /* the newest sample's time; a step's; ticks carried; the last frame's real time */
    double anchor, irate; int wok;            /* the creator's clock minus the wall clock; the drift learnt; steering */
    uint32_t samples, hard, steps; double err_sum, err_max; uint32_t frames;   /* for the log */
    double rtt_min;
    struct { uint32_t rt; double wall; } sent[64]; int sh;   /* our car states' real-time stamps, and when they went */
} K;
static double wall_s(void){ return (double)now_us() * 1e-6; }
/* the line through n points (x the time from now), a its value now, b its slope; b given when n < 4 or close in time */
static void clock_line(const double *x, const double *y, int n, double b0, double *a, double *b){
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int i = 0; i < n; i++){ sx += x[i]; sy += y[i]; sxx += x[i] * x[i]; sxy += x[i] * y[i]; }
    double mx = sx / n, my = sy / n, vx = sxx / n - mx * mx;
    *b = b0;
    if (n >= 4 && vx > 0.01 * 0.01){                    /* (spread over 10 ms and more) */
        double k = (sxy / n - mx * my) / vx;
        *b = k > CK_DRIFT ? CK_DRIFT : k < -CK_DRIFT ? -CK_DRIFT : k;
    }
    *a = my - *b * mx;
}
/* One sample (0x5891B0's): the wall clock r when the creator's answer came,
 * the round trip (from when our state went, clock_sent), the creator's clock
 * then (its race clock plus half the round trip) minus r.  The estimate: the line through the last CK_FRESH
 * s of the samples whose round trip was within 4 ms (or 15 %) of the least
 * over CK_SPAN s -- a waited answer (a queue, a held datagram, the creator's
 * frame) says the clock is later than it is by half the wait.  A line: a
 * creator's world that runs slower than real time (frames over the game's
 * dt clamp) drifts.  A step (the creator's world stood): the newest 3 all
 * CK_STEP off the line of the ones before, on one side, start it anew */
static void clock_sample(double r, double rtt, double th){
    K.s[K.h].r = r; K.s[K.h].rtt = rtt; K.s[K.h].th = th;
    K.h = (K.h + 1) % CK_N; if (K.n < CK_N) K.n++;
    K.samples++;
    double mn = 1e9;
    for (int k = 0; k < K.n; k++) if (r - K.s[k].r <= CK_SPAN && K.s[k].rtt < mn) mn = K.s[k].rtt;
    double lim = mn + (0.15 * mn > 0.004 ? 0.15 * mn : 0.004), x[24], y[24], best = 1e9, bx = 0, by = th;
    int m = 0;
    for (int i = 1; i <= K.n; i++){                     /* newest first */
        int k = (K.h - i + CK_N) % CK_N;
        if (r - K.s[k].r > CK_FRESH || K.s[k].r < K.since) break;
        if (K.s[k].rtt < best){ best = K.s[k].rtt; bx = K.s[k].r - r; by = K.s[k].th; }
        if (K.s[k].rtt <= lim && m < 24){ x[m] = K.s[k].r - r; y[m] = K.s[k].th; m++; }
    }
    double a, b;
    if (!m){ a = by - K.drift * bx; b = K.drift; }
    else {
        clock_line(x, y, m, K.drift, &a, &b);
        if (m >= 6){
            double a0, b0;
            clock_line(x + 3, y + 3, m - 3, K.drift, &a0, &b0);
            int side = 0, all = 1;
            for (int i = 0; i < 3; i++){
                double e = y[i] - (a0 + b0 * x[i]);
                int sd = e > CK_STEP ? 1 : e < -CK_STEP ? -1 : 0;
                if (!sd || (side && sd != side)) all = 0;
                side = sd;
            }
            if (all){
                K.since = r + x[2] - 1e-6; K.steps++;
                clock_line(x, y, 3, K.drift, &a, &b);
            }
        }
    }
    K.th = a; K.drift = b; K.at = r; K.have = 1; K.rtt_min = mn;
}
static uint32_t bits_guest(uint32_t bs, uint32_t pos, uint32_t nb){
    uint32_t v = 0;
    for (uint32_t i = 0; i < nb; i++, pos++) v = v << 1 | (uint32_t)(MR8(bs + (pos >> 3)) >> (7u - (pos & 7u)) & 1u);
    return v;
}
/* A car state of ours going to the creator (0x585D50's bit stream: 8 bits
 * the message, 32 the race clock, the slot, 32 our real-time clock): when it
 * went by the wall clock, for the round trip of the creator's answer, which
 * echoes that real-time stamp.  The game's real-time clock stands through a
 * long frame as its world does; the wall clock goes on */
static void clock_sent(uint32_t bs){
    uint32_t nb = guest_rd32(G_SLOTBITS);
    if (!clock_on() || is_host() || !guest_ok(bs, BS_BYTES + 4u) || nb > 8u || MR32(bs + BS_BYTES) < 80u + nb) return;
    K.sent[K.sh].rt = bits_guest(bs, 40u + nb, 32u); K.sent[K.sh].wall = wall_s();
    K.sh = (K.sh + 1) % 64;
}
/* The creator's answer to a joiner's car state (cdecl: the bit stream; at its
 * read position 32 bits our real-time clock echoed, 32 bits the creator's race
 * clock, then the creator's car): the game's own, but the race clock stays
 * ours (clock_sample, clock_frame): the game set WorldTimer to one sample's
 * clock (0.25 s off at once, smaller after 5 in a row on one side) and ran
 * the world 0.85-1.2 times as fast until it got there */
void sub_005891b0_orig(CPU *c);
void sub_005891b0(CPU *c){
    uint32_t bs = MR32(c->esp + 4u);
    if (!clock_on() || !guest_ok(bs, BS_READ + 4u)){ sub_005891b0_orig(c); return; }
    uint32_t pos = MR32(bs + BS_READ);
    if (pos + 64u > 8u * BS_BYTES){ sub_005891b0_orig(c); return; }
    union { uint32_t u; float f; } f1 = { bits_guest(bs, pos, 32u) }, f2 = { bits_guest(bs, pos + 32u, 32u) };
    float echo = MRF32(G_CLK_ECHO);
    uint32_t tgt = MR32(G_CLK_TARGET); uint8_t flag = MR8(G_CLK_FLAG);
    sub_005891b0_orig(c);
    if (in_race()){ MR32(G_CLK_TARGET) = tgt; MR8(G_CLK_FLAG) = flag; }   /* before the race the game's own (its start) */
    if (!(f1.f > echo)) return;                         /* the game took none of it either */
    double r = wall_s(), sent = -1;
    for (int i = 0; i < 64; i++) if (K.sent[i].rt == f1.u && K.sent[i].wall > 0){ sent = K.sent[i].wall; break; }
    if (sent < 0) return;                               /* not one of ours we know (an older game, long ago) */
    double rtt = r - sent;
    if (!(rtt >= 0.0 && rtt < 5.0) || !__builtin_isfinite(f2.f)) return;
    clock_sample(r, rtt, (double)f2.f + 0.5 * rtt - r);
}
/* Every main-loop pass of a network race (racing: before it the game's own
 * sync sets the start): WorldTimer to a clock on the wall clock -- on the
 * creator its own from the race's start (its clock is everyone's: a creator's
 * long frames used to set every joiner's clock back, and the cars' states'
 * stamps with them), on a joiner the creator's estimate -- with CK_TAU and
 * what it keeps drifting by learnt (CK_TAU_I: a world a little slower than
 * the wall clock), at most a fifth of the pass's time a pass, an error over
 * CK_HARD at once.  A frame of the game can be several passes (its catch-up
 * steps, one right after the other), so WorldTimer runs in steps around the
 * wall clock: the time constant evens them out.  Only the clock moves (the
 * world's step stays the game's: the cars, ours too, at their speed) */
static void clock_put(uint32_t wt, double d){
    K.frac += d * 4000.0;
    int32_t n = (int32_t)K.frac;
    if (n){ MR32(G_WORLDTIMER) = wt + (uint32_t)n; K.frac -= n; MRF32(G_WORLDTIME_S) = (float)((double)(wt + (uint32_t)n) * 0.00025); }
    K.wok = 1;
}
static void clock_frame(void){
    double r = wall_s(), fdt = K.prev_r > 0 ? r - K.prev_r : 0;
    K.prev_r = r;
    uint32_t wt = guest_rd32(G_WORLDTIMER);
    if (!(guest_rd32(G_GAMEMODE) & 0x208u)){ memset(&K, 0, sizeof K); return; }
    if (!clock_on() || !in_race()){ K.wok = 0; K.irate = 0; return; }
    if (!(fdt > 0 && fdt < 1.0)) fdt = 0;
    double target;
    if (is_host()){
        if (!K.wok){ K.anchor = ((double)wt + K.frac) * 0.00025 - r; K.irate = 0; }
        target = r + K.anchor;
    } else {
        if (!K.have || r - K.at > 2.0){ K.wok = 0; return; }
        target = r + K.th + K.drift * (r - K.at);
    }
    double e = target - ((double)wt + K.frac) * 0.00025, ae = e < 0 ? -e : e;
    K.frames++; K.err_sum += ae; if (ae > K.err_max) K.err_max = ae;
    if (ae > CK_HARD){
        double t = target * 4000.0;
        if (!(t > 0 && t < 4.0e9)){ K.wok = 0; return; }
        MR32(G_WORLDTIMER) = (uint32_t)(t + 0.5); MRF32(G_WORLDTIME_S) = (float)((double)MR32(G_WORLDTIMER) * 0.00025);
        K.frac = 0; K.hard++; K.wok = 1;
        return;
    }
    if (ae < CK_ILIM) K.irate += e * fdt / (CK_TAU * CK_TAU_I);   /* (not while a step is taken back) */
    if (K.irate > CK_DRIFT) K.irate = CK_DRIFT;
    if (K.irate < -CK_DRIFT) K.irate = -CK_DRIFT;
    double d = (e / CK_TAU + K.irate) * fdt, cap = 0.2 * fdt;
    if (d > e && e >= 0) d = e;
    if (d < e && e < 0) d = e;
    if (d > cap) d = cap;
    if (d < -cap) d = -cap;
    clock_put(wt, d);
}

/* ---- the car states' rates (above) ---- */
/* In a network race (Online, LAN) every field group at rate_hz(); the
 * game's own (its tweak data's, 0x5F0075) back after it */
static void rates_frame(void){
    static uint32_t was[4]; static int set;
    if (!guest_ok(G_RATES, 15u)) return;
    int want = (guest_rd32(G_GAMEMODE) & 0x208u) && rate_hz() > 0;
    if (want && !set){ for (uint32_t k = 0; k < 4; k++) was[k] = MR32(G_RATES + 4u * k); set = 1; }
    if (want) for (uint32_t k = 0; k < 4; k++){ if (MR32(G_RATES + 4u * k) != (uint32_t)rate_hz()) MR32(G_RATES + 4u * k) = (uint32_t)rate_hz(); }
    else if (set){ for (uint32_t k = 0; k < 4; k++) MR32(G_RATES + 4u * k) = was[k]; set = 0; }
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
    f->clock += ((double)theirs - (double)guest_rd32(G_WORLDTIMER)) / 4.0;   /* ms, the way's delay in it */
}

/* this game's player has finished the race (or left it: the coordinator's
 * own racer entries) */
static int finished(uint32_t rc, uint32_t me){
    uint32_t n = guest_rd32(rc + 0x24u);
    for (uint32_t i = 0; i < n && i < 8; i++){
        uint32_t r = guest_rd32(rc + 0xbd0u + 4u * i);
        if (r && guest_rd32(r) == me) return guest_ok(r, RCR_DONE + 2u) && MR16(r + RCR_DONE) != 0;
    }
    return 0;
}
static void begin(uint64_t t, uint32_t me){
    memset(&R, 0, sizeof R);
    memset(g_car, 0, sizeof g_car);
    memset(g_fps, 0, sizeof g_fps);
    R.on = 1; R.t0 = R.w0 = t;
    R.host = (int32_t)guest_rd32(G_NET_HOST) > 0;
    car_name(me, R.me);
    vlan_race_links(0, 0, 1);
    K.samples = K.hard = K.steps = K.frames = 0; K.err_sum = K.err_max = 0;
    fprintf(stderr, "[racenet] race begins: me %s, %s; car states %s, race clock %s, cars %s\n", R.me[0] ? R.me : "(no name)",
            R.host ? "the game's creator" : "a joiner (every car comes through the creator)",
            rate_hz() ? "every field each time" : "the game's rates", clock_on() ? "ours" : "the game's",
            predict_on() ? "where their owners have them now" : stall_s() > 0.f ? "the game's blend, no brake in a stall" : "the game's blend");
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
                "the creator's list %s (%u other joiners), %u lists from elsewhere refused\n", R.cs_sent, R.cs_came, R.cs_refused,
                !direct_on() ? "not used (NFSU2_RACE_DIRECT=0)" : !g_pl.at ? "never came (an older version)" :
                g_pl.slots ? "came with their slots" : "came without their slots (an older version)", g_pl.n,
                R.pl_refused);
    if (!R.host && clock_on())
        fprintf(stderr, "[racenet]   race clock (ours): %u of the creator's answers, the least round trip %.0f ms, its clock's steps %u; "
                "off the estimate %.1f ms on average, at most %.1f ms; %u set at once (over %.0f ms); drifting %+.2f %%\n",
                K.samples, K.rtt_min * 1e3, K.steps, K.frames ? K.err_sum / K.frames * 1e3 : 0., K.err_max * 1e3, K.hard, CK_HARD * 1e3, K.irate * 100);
    else if (clock_on())
        fprintf(stderr, "[racenet]   race clock (ours, everyone's): on the wall clock, off it %.1f ms on average, at most %.1f ms; "
                "%u set at once; our world drifting %+.2f %%\n", K.frames ? K.err_sum / K.frames * 1e3 : 0., K.err_max * 1e3, K.hard, K.irate * 100);
    for (int i = 0; i < NCARS; i++){
        Car *e = &g_car[i];
        if (!e->car) continue;
        Pred *pp = predict_on() ? pred_of(e->car, 0) : 0;
        if (pp && pp->steps + pp->own_steps)
            fprintf(stderr, "[racenet]   car %s where its owner has it: %u steps, %u on its own (a hit here, a stall), "
                    "carried on up to %.0f ms, %u glides back (largest %.1f m), %u snaps; %u states with an older game's repeated velocity\n",
                    e->name[0] ? e->name : "-", pp->steps, pp->own_steps, (double)pp->carried_max * 1e3, pp->glides, (double)pp->glide_max,
                    pp->snaps, pp->old_states);
        double span = e->n > 1 ? (double)(e->last - e->first) / 1e6 : 0.;
        fprintf(stderr, "[racenet]   car %s: %u updates (%.1f/s, %u straight from their game), longest wait %.0f ms, "
                "waits over 100/250/500 ms %u/%u/%u, jumps over 0.5 m %u, over 2 m %u (largest %.1f m), "
                "as the game had them %u/%u (largest %.1f m; %.1f m past a stall's), corrections faded %u, shown %u, "
                "far \"hits\" passed %u, its states %.0f ms old by our race clock (%.0f to %.0f)",
                e->name[0] ? e->name : "-", e->n, span > 0 ? (e->n - 1) / span : 0., e->direct, (double)e->gap_max / 1000.0,
                e->g100, e->g250, e->g500, e->shown.jumps, e->shown.big, (double)e->shown.max,
                e->game.jumps, e->game.big, (double)e->game.max, (double)e->jump, e->faded, e->kept, e->unhit,
                e->n ? e->age_sum / e->n * 1000.0 : 0., (double)e->age_min * 1000.0, (double)e->age_max * 1000.0);
        Fps *f = 0;
        for (int j = 0; j < NCARS && e->name[0]; j++) if (!strcmp(g_fps[j].name, e->name)) f = &g_fps[j];
        if (f && f->n) fprintf(stderr, "; their game %.1f fps (lowest %.1f), %u frames over 50 ms (longest %u ms), "
                               "their race clock %+.0f ms off ours\n", (double)(f->sum / (float)f->n), (double)f->lo,
                               f->slow, f->longest_ms, f->clock / f->n);
        else fprintf(stderr, "; their frame rate unknown (an older version)\n");
    }
    racecars_log(s);
    R.on = 0;
}

/* a car's place this frame into its jumps (racenet_frame): the jump's length when this frame began one (m), else 0 */
static float jumps_add(Jumps *j, const float *p, uint64_t dt, float step){
    float d[3], got = 0.f;
    for (int i = 0; i < 3; i++) d[i] = p[i] - j->p[i];
    if (j->have > 1 && dt && j->step > 0.f){
        float s = step / j->step, x2 = 0;
        for (int i = 0; i < 3; i++) x2 += (d[i] - j->d[i] * s) * (d[i] - j->d[i] * s);
        float x = __builtin_sqrtf(x2);
        int jump = __builtin_isfinite(x) && x > 0.5f;
        if (jump && !j->jumped){ j->jumps++; j->big += x > 2.f; }
        if (jump && x > j->max) j->max = x;
        if (jump && !j->jumped) got = x;   /* (a jump's next frame, back to the car's own move, differs as much) */
        j->jumped = jump;
    }
    if (j->have) memcpy(j->d, d, sizeof d);
    memcpy(j->p, p, sizeof j->p); j->step = step; j->have += j->have < 2;
    return got;
}

/* every frame, on the game thread (roam.c's main-loop hook) */
void racenet_frame(CPU *c){
    (void)c;
    uint32_t pl = guest_rd32(G_PLAYER1), me = pl ? guest_rd32(pl + 4u) : 0, w = guest_rd32(G_WORLD), rc = guest_rd32(G_RACE);
    int racing = guest_rd32(G_FLOW) == 6u && (guest_rd32(G_GAMEMODE) & 0x208u) && guest_ok(me, 0x9e0) && guest_ok(w, 0x80) && guest_rd32(rc) == RC_RACING &&
                 !finished(rc, me);
    uint64_t t = now_us();
    rates_frame();
    clock_frame();
    if (racing && !R.on) begin(t, me);
    else if (!racing && R.on) end(t);
    racecars_frame(c);                               /* (after end: its lines first) */
    /* the creator's list of the joiners, the cars drawn (also once our own
     * player is through: the others still race) */
    if (direct_on() && is_host() && in_race() && ms_now() - g_pl_sent >= PL_MS){ g_pl_sent = ms_now(); list_send(); }
    if (!in_race() && (g_pl.from || g_nbind)) direct_reset();
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
    static uint32_t clk_prev; uint32_t clk = guest_rd32(G_WORLDTIMER);
    float drt = clk_prev ? (float)(int32_t)(clk - clk_prev) * 0.00025f : 0.f;
    clk_prev = clk;
    uint32_t car = guest_rd32(w);
    for (int k = 0; car && car != w && k < 64 && guest_ok(car, 0x9e0); k++, car = guest_rd32(car)){
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
        float x = jumps_add(&e->game, g, dt, step);
        /* (the race's report: a jump right after a wait of over STALL_US is the car shown where it went meanwhile, as
         * far as a car drives in that time) */
        if (x > 0.f && e->gap_last > STALL_US && t - e->gap_at < STALL_AFTER_US) x -= STALL_MPS * (float)e->gap_last / 1e6f;
        if (x > e->jump) e->jump = x;
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
        uint32_t clk = guest_rd32(G_WORLDTIMER);
        m[12] = (uint8_t)clk; m[13] = (uint8_t)(clk >> 8); m[14] = (uint8_t)(clk >> 16); m[15] = (uint8_t)(clk >> 24);
        memcpy(m + 16, R.me, strlen(R.me));
        send_all(m, 0);
        R.w0 = t; R.wframes = 0; R.wslow = 0; R.wlongest = 0;
    }
}

/* the largest jump of the car of the player named so (its LAN name) in the race just raced, as our game had it, not
 * right after a stall (m; 0: none, or no such car) */
/* NFSU2_TEST_JUMP=<name>:<m> or <name>/<m> (tests, with NFSU2_INPUT_SCRIPT only; roam-pair splits its options at
 * ':'): that player's car jumped so far */
static float test_jump(const char *name){
    const char *tj = getenv("NFSU2_TEST_JUMP"), *sc = getenv("NFSU2_INPUT_SCRIPT"), *c = tj ? strpbrk(tj, ":/") : 0;
    if (c && sc && sc[0] && (size_t)(c - tj) == strlen(name) && !strncasecmp(tj, name, (size_t)(c - tj))) return (float)atof(c + 1);
    return 0.f;
}
float racenet_jump(const char *name){
    if (!name || !name[0]) return 0.f;
    float t = test_jump(name);
    if (t > 0.f) return t;
    for (int i = 0; i < NCARS; i++) if (g_car[i].car && !strcasecmp(g_car[i].name, name)) return g_car[i].jump;
    return 0.f;
}
float racenet_farthest(char name[16]){
    float far = 0.f; int at = -1;
    for (int i = 0; i < NCARS; i++){
        Car *e = &g_car[i];
        if (!e->car || !e->name[0]) continue;
        float j = test_jump(e->name);
        if (!(j > 0.f)) j = e->jump;
        if (j > far){ far = j; at = i; }
    }
    if (name) snprintf(name, 16, "%s", at >= 0 ? g_car[at].name : "");
    return at >= 0 ? far : 0.f;
}
