/* mprace.h -- a multiplayer race from free roam: the trip from the career's
 * free roam to the game's own LAN race and back (mprace.c), which the lobby
 * (roam_game.c mp_*) starts in every game of its players at once. */
#ifndef MPRACE_H
#define MPRACE_H
#include <stdint.h>

typedef struct {
    int      host;                 /* 1: this game hosts the LAN game; 0: it joins the host's */
    uint32_t host_ip;              /* the host's virtual address (10.77.x.y, host byte order) */
    char     host_name[16];        /* the host's LAN name (its game's name) */
    char     name[16];             /* our LAN name: letters, digits, - and _ */
    char     password[12];         /* the game's password: only the lobby's players join it */
    uint32_t race;                 /* the career race (its name hash, race record +8): for the log and the prize */
    int      mode;                 /* the network race's type [0x850088]: 0 circuit, 1 sprint, 2 drag, 3 drift, 4 URL, 0xA Street X */
    int      track, laps, reverse;
    int      players;              /* the host: the lobby's players, itself too (it starts once they are all ready) */
    uint32_t credit;               /* the career race's record when a win counts in this player's career (roam_game.c
                                    * mp_counts: a race of the player's stage not won yet, its own gates met), else 0 */
    int      heats;                /* the race's heats (race record +0x7E: a URL race has 1 to 3); 1 for any other */
    int      nfriends;             /* the lobby's players who are our friends or crew-mates (the hub's marks, at the */
    char     friends[6][16];       /* trip's start): their LAN names.  Their win counts for us too (MpRace.credit) */
    int      host_up;              /* a joiner: the host's LAN server takes logins already (its lobby said so):
                                    * connect at once (mprace_host_up when that comes later) */
} MpRace;
enum { MPR_IDLE = 0, MPR_OUT, MPR_LAN, MPR_RACE, MPR_BACK, MPR_DONE, MPR_FAILED };
/* what quitting a started race with its pause menu's Quit costs the bank (what it holds at most): this share of what
 * 1st place pays the player (mprace_prize_of(1), the player's stage's): $75 at stages 1 to 3 ($400 for a win), $225
 * at stage 4 ($1200), $375 at stage 5 ($2000) */
#define MPRACE_QUIT_SHARE_NUM 3
#define MPRACE_QUIT_SHARE_DEN 16
int  mprace_quit_fee(void);
/* the least prize money of 1st to 4th (mprace_prize_of) */
#define MPRACE_PRIZE_FLOOR { 400, 300, 200, 100 }
/* a race's evening out (mprace.c balance_race): a car's engine power at most this much up, and down; the faster
 * cars' share of the way (their target between the cars' middle and the best); how much of its way to the target
 * each car's rating goes (the rest of the gap stays: the better car stays a little ahead), and in a drag race (where
 * the smallest edge decides) */
#define MPRACE_BALANCE_UP    0.50f
#define MPRACE_BALANCE_DOWN  0.25f
#define MPRACE_BALANCE_SHARE (1.f / 3.f)
#define MPRACE_BALANCE_CLOSE 0.90f
#define MPRACE_BALANCE_CLOSE_DRAG 1.00f

/* the career has a car of its own, one the LAN race can take (not a new
 * career's prologue car): a trip without one would race a car made of
 * garbage */
int  mprace_car_ok(void);
/* the trip starts at the next frame; 0 when one runs already, the game is
 * not in career free roam or the career has no car of its own */
int  mprace_start(const MpRace *r);
/* where the trip is (MPR_*); MPR_DONE / MPR_FAILED until the next start or
 * mprace_ack */
int  mprace_phase(void);
void mprace_ack(void);
/* our place in the race (1 ..), 0: none (not finished, left, failed); the
 * prize the career's bank got for it (0 when the race counted in the career
 * instead: mprace_credit) */
int  mprace_place(void);
int  mprace_prize(void);
/* the prize money of a place (1 ..) in a race that does not count in the career: the career's normal race of the
 * player's stage pays the winner, the next three 3/4, 1/2 and 1/4 of that, never less than MPRACE_PRIZE_FLOOR */
int  mprace_prize_of(int place);
/* the race in the career: MPC_NONE, MPC_WON (we won it), MPC_FRIEND (a friend won it, we finished); MPC_DONE once the
 * career has it (the crib: the game's own win, mpcredit.c), MPC_FAILED when the game would not take it */
enum { MPC_NONE = 0, MPC_WON, MPC_FRIEND };
int  mprace_credit(void);          /* MPC_* */
int  mprace_credited(void);        /* 1 the career has the win, -1 it would not take it, 0 not yet / none */
int  mprace_counts(void);          /* 1: a win of this race counted in our career (whether we won it or not) */
/* the prize money paid on top of the career's own prize, where the place's (mprace_prize_of) is more */
int  mprace_topup(void);
/* the race's winner (its LAN name, "" none) */
const char *mprace_winner(void);
/* the race's results as the game showed them (its results' rows, in their order): each racer's LAN name, whether it
 * finished, its time (s) or, in a drift race, its points, its row's time whether it finished or not (s: one still on
 * the way when the race ended, 30 s after the first, has that), how far it came (laps), the way it drove (m: its
 * average speed for its time) and which is ours; how many (0: no results) */
#define MPRACE_RESULTS_MAX 6
typedef struct { char name[16]; int finished, ours, points; float time, clock, done, dist; } MpResult;
int  mprace_results(MpResult *r, int max);
/* a URL race of 2 or 3 heats, raced one after the other in one trip (mprace.c): the heat being raced, or the last one
 * raced (1 ..), of heats; final: the last heat's results are in (mprace_results is the series' standing then, each
 * row's points the series'); the players by their points so far, the leader first.  0 for a race of one heat */
typedef struct { int heat, heats, final, n; struct { char name[16]; int points, ours; } p[MPRACE_RESULTS_MAX]; } MpSeries;
int  mprace_series(MpSeries *s);
/* the rows of the race's last heat as the game showed them (its times; mprace_results for a race of one heat) */
int  mprace_heat_results(MpResult *r, int max);
/* our car's engine power in the race as evened out (1: as it was) */
float mprace_factor(void);
/* a network race of this type (its N_MODE: circuit 0, sprint 1, drag 2, drift 3, URL 4, Street X 10) evens
 * out the cars */
int  mprace_evened(int net_mode);
/* the player quit the race (its pause menu's Quit); the fee the bank paid for it (less than mprace_quit_fee when it
 * held less, 0 before the crib) */
int  mprace_quit(void);
int  mprace_fee(void);
/* (racehorn.c, from the horn's port) an "N2QT": a racer of our race quit it with its pause menu's Quit (mprace.c) */
void mprace_quit_heard(uint8_t *m, int n, uint32_t from);
/* the reason a failed trip gives ("the host's game was not found") */
const char *mprace_why(void);
/* a joiner's host closed its game (free roam heard it quit): the trip ends
 * once in the LAN menus, as the host's LAN server is gone with it */
void mprace_host_gone(void);
/* the host: a player of the lobby is not coming after all (its trip did not start, it quit): the room does not wait
 * for it; with nobody else left the trip ends */
void mprace_player_gone(void);
/* a joiner: the host's lobby says its LAN server takes logins now: the trip connects to it at once (it waited for
 * the server's LAN broadcast, which on a crowded switch never came, and then 12 s) */
void mprace_host_up(void);
/* the trip gave up before its race (it is on its way back, or over): the others are told (roam_game.c) */
int  mprace_gave_up(void);
struct CPU;
/* The host's LAN server, started early: while its lobby waits in free roam, as the game's Host LAN Server starts it
 * (server.dll, StartLANServer 0x5FA300 with the host's LAN name).  On the trip the game's own start finds it running
 * and starts nothing, and the host's login need not sit out the game's 5 s from that start (the server takes
 * logins about 2.5 s after its start): it goes once 5 s have passed since this one.  1 when it runs */
int  mprace_server_start(struct CPU *c, const char *name);
/* ...stopped (unloaded, as the game's leaving LAN does) unless a trip took it over: the lobby ended without its race */
void mprace_server_stop(struct CPU *c);
/* the host: its LAN server takes logins (started early 5 s ago or more, or the host logged in on it): the others
 * may connect */
int  mprace_lan_up(void);
/* the player is in career free roam again while the trip is not over (it made its way back by hand, or the trip
 * lost its way): the trip is over, as done once raced (the prize, if the crib did not pay it, into the bank now),
 * else as failed */
struct CPU;
void mprace_end_in_roam(struct CPU *c);
/* the game's input is the trip's own: the player's keys, mouse and pad are
 * not passed on (runtime/web/input_web.c, dinput_pad.c) */
int  mprace_owns_input(void);
/* the trip is in the game's menus (out of free roam, the LAN screens, the
 * race's loading, the way back): the page covers them (the lobby's panel,
 * roam_game.c mp_panel); not in the race, not on its results */
int  mprace_covered(void);
/* where the trip is, for the player: "Leaving free roam", "Loading the race", ... */
const char *mprace_step(void);

/* A car's performance as the game rates it (its physics info with the
 * performance parts in, mprace.c): the class and rating the game gave it,
 * the top speed its push and drag allow, its mass, the engine's torque
 * curve and the power from it */
typedef struct {
    int      type;                 /* CarTypeInfo index */
    char     name[24];             /* the type's name without CAR_NAME_ */
    int      cls;                  /* the game's class: 0 fastest .. 3, -1 not rated */
    float    rating;               /* the game's rating: the push beyond drag over the speeds (class 0 above 500, 1 above 401, 2 above 273) */
    float    top;                  /* top speed, m/s */
    float    mass;                 /* kg, the weight upgrade's in (the game has tonnes) */
    float    rpm_idle, rpm_max;
    float    torque[9];            /* N m (the game has kN m) at rpm_idle + i (rpm_max - rpm_idle) / 8 */
    float    power;                /* the curve's most, W */
} CarPerf;
/* the performance of a car (a car object: its physics info [car + 0x10] +
 * 0x10); 0 when it has none */
int  mprace_car_perf(uint32_t car, CarPerf *p);
/* only its class (0 fastest .. 3; -1: no physics info), cheap enough for every frame */
int  mprace_car_class(uint32_t car);
/* one log line: who's car and its numbers */
void mprace_perf_log(const char *who, uint32_t car);
/* the engine's power times f: all of its torque curve (the turbo's is in it)
 * and its nitrous (the engine reads both in place); 0 when the car has no
 * physics info */
int  mprace_torque_scale(uint32_t car, float f);
/* (test aid) the car's governor at mph (0: none, as with an ECU part); 0 when the car has no physics info */
int  mprace_speed_limit(uint32_t car, float mph);
struct CPU;
/* test aid: the player's car rated as every player car type, stock and with
 * every performance part, then put back (roam mp perf survey) */
void mprace_perf_survey(struct CPU *c, uint32_t car);
/* 1 when player car type `type`'s record in GLOBAL\GlobalB (its physics and
 * parts' tables) is not the game's own: edited game data */
int  mprace_type_edited(int type);
/* an Online race's rank frame's code (olfilter.c): "ed" and the type of the
 * car we drive in two hex digits when its data is edited ("edff": a type is
 * edited and we have no car now); 0 (code untouched) when not */
int  mprace_rank_code(char code[4]);
/* the last trip's race, for its report to the server (roam_game.c, RC_MSG_RACEREP): the network race's type, track
 * and laps, its cars and their average rating, our place (0: not finished), whether we hosted it or quit it, our car
 * as rated in the race, its engine power's factor there and the race's time (s, 0: none) */
typedef struct {
    int      mode, track, laps, cars, place, host, quit;
    float    field, factor, time;
    CarPerf  car;
} MpReport;
/* 1 when the trip raced and rated its cars (the report filled in) */
int  mprace_report(MpReport *r);
#endif
