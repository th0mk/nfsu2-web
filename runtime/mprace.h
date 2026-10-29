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
} MpRace;
enum { MPR_IDLE = 0, MPR_OUT, MPR_LAN, MPR_RACE, MPR_BACK, MPR_DONE, MPR_FAILED };
/* what quitting a started race with its pause menu's Quit costs the bank (what it holds at most) */
#define MPRACE_QUIT_FEE 75
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
 * prize the career's bank got for it */
int  mprace_place(void);
int  mprace_prize(void);
/* the race's results as the game showed them (its results' rows, in their order): each racer's LAN name, whether it
 * finished, its time (s) or, in a drift race, its points, its row's time whether it finished or not (s: one still on
 * the way when the race ended, 30 s after the first, has that), how far it came (laps), the way it drove (m: its
 * average speed for its time) and which is ours; how many (0: no results) */
#define MPRACE_RESULTS_MAX 6
typedef struct { char name[16]; int finished, ours, points; float time, clock, done, dist; } MpResult;
int  mprace_results(MpResult *r, int max);
/* our car's engine power in the race as evened out (1: as it was) */
float mprace_factor(void);
/* a network race of this type (its N_MODE: circuit 0, sprint 1, drag 2, drift 3, URL 4, Street X 10) evens
 * out the cars */
int  mprace_evened(int net_mode);
/* the player quit the race (its pause menu's Quit); the fee the bank paid for it (less than MPRACE_QUIT_FEE when it
 * held less, 0 before the crib) */
int  mprace_quit(void);
int  mprace_fee(void);
/* the reason a failed trip gives ("the host's game was not found") */
const char *mprace_why(void);
/* a joiner's host closed its game (free roam heard it quit): the trip ends
 * once in the LAN menus, as the host's LAN server is gone with it */
void mprace_host_gone(void);
/* the host: a player of the lobby is not coming after all (its trip did not start, it quit): the room does not wait
 * for it; with nobody else left the trip ends */
void mprace_player_gone(void);
/* the player is in career free roam again while the trip is not over (it made its way back by hand, or the trip
 * lost its way): the trip is over, as done once raced (the prize, if the crib did not pay it, into the bank now),
 * else as failed */
void mprace_end_in_roam(void);
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
