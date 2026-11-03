/* The page's frame-rate overlay reads the game's own counters from here
 * (HOST_KIND_STATUS, layout in status_proto.h).  A small thread refreshes
 * the renderer's counters four times a second; other parts of the web
 * runtime may set their own with host_status_set().  With the null renderer
 * "frames" still counts the game's presents, so the overlay shows the game's
 * frame rate rather than the page's display rate. */
#include "cpu.h"
#include "status_proto.h"
#include "host_abi.h"
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

long metal_frames(void); long metal_tris(void); long metal_tex_created(void);
unsigned long metal_tex_released(void); long metal_frames_dropped(void);

static HostStatus g_st;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

int host_status_slot(const char *name){
    pthread_mutex_lock(&g_mu);
    int i = 0;
    for (; i < (int)g_st.count; i++) if (!strncmp(g_st.item[i].name, name, HOST_STATUS_NAME)) break;
    if (i == (int)g_st.count){
        if (i >= HOST_STATUS_MAX){ pthread_mutex_unlock(&g_mu); return -1; }
        __atomic_add_fetch(&g_st.seq, 1, __ATOMIC_RELEASE);
        strncpy(g_st.item[i].name, name, HOST_STATUS_NAME);
        g_st.item[i].value = 0;
        __atomic_store_n(&g_st.count, (uint32_t)i + 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_st.seq, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_mu);
    return i;
}

void host_status_set(int slot, double value){ host_status_set_n(&slot, &value, 1); }

/* several counters in one sequence-lock section, so the page never sees one
 * of them newer than another (frames and frames_t below) */
void host_status_set_n(const int *slot, const double *value, int n){
    pthread_mutex_lock(&g_mu);
    __atomic_add_fetch(&g_st.seq, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < n; i++) if (slot[i] >= 0 && slot[i] < HOST_STATUS_MAX) g_st.item[slot[i]].value = value[i];
    __atomic_add_fetch(&g_st.seq, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_mu);
}

HostStatusRadio *host_status_radio(void){ return &g_st.radio; }

/* "frames_t" is the time "frames" was read, in ms on CLOCK_MONOTONIC (the
 * page's performance.timeOrigin + now(), the same clock).  The page divides
 * the change in frames by the change in frames_t: its own sampling times
 * (animation frames, about every 250 ms) do not line up with this thread's
 * updates, and dividing by them misread a steady 60 fps as 56 or 75 (phase-1
 * app review, finding 1). */
static void *status_main(void *unused){
    (void)unused;
    int slot[5] = { host_status_slot("frames"), host_status_slot("frames_t"), host_status_slot("frames_dropped"),
                    host_status_slot("tris"), host_status_slot("textures") };
    for (;;){
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        double v[5] = { (double)metal_frames(), ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6, (double)metal_frames_dropped(),
                        (double)metal_tris(), (double)metal_tex_created() - (double)metal_tex_released() };
        host_status_set_n(slot, v, 5);
        usleep(g_st.period_ms * 1000u);
    }
    return 0;
}

/* "drive": what the page's touch controls (web/src/app/touch.ts) need of the
 * game, set at every Present (autotest.c): 1 while the player drives in a
 * race (the crib's test runs too: roam.c roam_exploring), 2 in free roam,
 * 0 elsewhere, plus 4 with a manual gearbox and 8
 * during a cutscene (the touch controls' level horizon waits).  Driving
 * is GameFlow 6 with a race HUD (HUD_*.fng) the top package; the pause menu
 * (UI_pause.fng), the results, the world's loading screen and the Engage
 * dialog over the HUD are not, nor is a cutscene in free roam (NIS, as
 * runtime/discord.c tells one).  A race's start is one too, the camera's
 * flight round the grid and the countdown until a moment after the start, so
 * a race drives during a cutscene: the player revs on the line and goes at
 * once (a player's report: the controls came seconds after the others left).
 * The gearbox is manual in a drag race (the game asks nothing there and sets
 * the car's gearbox to manual), else as the race's Transmission choice set
 * it: the player's setting 1 (0x4a7b30), kept at player 0 (0x8900ac) +0x20
 * and handed to the car's gearbox (0x407280: 0 and 2 shift by themselves, 1
 * and 3 keep the gear).  The gearbox itself is no good to read: in a drag
 * race the car's controls (+0x30) become the game's lane steering once it
 * rolls. */
#define G_PLAYER0    0x008900acu
#define PL_GEARBOX   0x20u
#define G_RACE_DRAG  0x0089e7d8u      /* race parameters (discord.c G_RACE) + R_DRAG */
const char *autotest_top_fng(void);
int career_fe_active(const char *name);
int roam_exploring(void);
static int manual_gearbox(int race){
    if (race && MR8(G_RACE_DRAG)) return 1;
    uint32_t p = mem_is_committed(G_PLAYER0 + 3u) ? MR32(G_PLAYER0) : 0u;
    if (!p || !mem_is_committed(p + PL_GEARBOX)) return 0;
    uint8_t g = MR8(p + PL_GEARBOX);
    return g == 1u || g == 3u;
}
static int drive_state(void){
    unsigned flow = mem_is_committed(0x8654a7u) ? MR32(0x8654a4u) : 0u;
    const char *top = autotest_top_fng();
    int hud = top && !strncasecmp(top, "HUD_", 4) && strcasecmp(top, "HUD_World_Loading.fng");
    int nis = mem_is_committed(0x827ecbu) && (MR32(0x827e98u) | MR32(0x827eb0u) | MR32(0x827ec8u));
    int engage = career_fe_active("UI_EngageMessageDialog.fng");
    int roam = roam_exploring();
    int v = flow == 6u && hud && !engage && !(roam && nis) ? (roam ? 2 : 1) : 0;
    if (v && manual_gearbox(v == 1)) v |= 4;
    if (v && nis) v |= 8;
    /* a line at each change, for a player's log */
    static unsigned l_flow = ~0u; static int l_hud = -1, l_nis = -1, l_engage = -1, l_v = -1;
    if (flow != l_flow || hud != l_hud || nis != l_nis || engage != l_engage || v != l_v){
        l_flow = flow; l_hud = hud; l_nis = nis; l_engage = engage; l_v = v;
        fprintf(stderr, "[drive] f=%ld flow=%u top=%s nis=%d engage=%d -> %d\n", metal_frames(), flow, top ? top : "-", nis, engage, v);
    }
    return v;
}
/* "rate": the frame rate the game paces to now (framerate.c content_rate:
 * the mode's rate, 30 in a cutscene, at most 120 in a network race; 0 =
 * uncapped), so the page can tell whether the game keeps up with it
 * (web/src/app/rendercheck.ts) */
int hfr_rate_now(void);
/* "pause": 1 while the game's pause menu (UI_Pause.fng) is the top package,
 * not its options or a question over it, so the page can point to its own
 * menu under the panel (web/src/app/pausehint.ts) */
static int pause_state(void){
    const char *top = autotest_top_fng();
    return top && !strcasecmp(top, "UI_Pause.fng");
}
/* "roam_cars", "roam_friends": friends' cars the game holds and friends
 * connected in free roam (roam_game.c), for the page's copy of the log
 * (web/src/app/lastgame.ts): what a phone held when Safari stopped the page.
 * The friends take the session lock, so every 30th frame. */
int roam_game_cars(void); int roam_game_friends(void);
static void roam_state(void){
    static int slot[2] = { -2, -2 }, n;
    static double last[2] = { -1, -1 };
    if (slot[0] == -2){ slot[0] = host_status_slot("roam_cars"); slot[1] = host_status_slot("roam_friends"); }
    double v[2] = { (double)roam_game_cars(), n++ % 30 ? last[1] : (double)roam_game_friends() };
    if (v[0] != last[0] || v[1] != last[1]){ last[0] = v[0]; last[1] = v[1]; host_status_set_n(slot, v, 2); }
}
/* "engage", "roam_near": for the touch controls' free-roam buttons
 * (web/src/app/touch.ts).  "engage" is 1 while Engage (Enter, the pad's
 * D-pad Up) does something in free roam, so Start shows only then: the
 * player's car on an event's or a shop's marker (player +0x9AC, what Engage
 * would enter), the game's own outrun offer (the career's outrun state 2,
 * set with the HUD's outrun icon by 0x500D20 and tested by 0x500D80), or one
 * of ours (roam_game_engage: a friend's outrun, a multiplayer-only race spot,
 * a lobby to join or to start).  "roam_near": friends' cars within a horn's
 * reach (roam_game_near), so Horn and Quick chat show only then.
 * "hydraulics": while the player drives a car with hydraulics, so the
 * Pressurize and Bounce buttons show only then: the level its Hydraulics part
 * gives it (RideInfo +0x581), or the game's own for every car when that is
 * not -1, as the driver's hydraulics controls (0x4078A0) read it.  2 while
 * a press of H or J would set them going, 1 while not: they start 0.22 s
 * after the press (0x407400) if the car is then at 10 mph or less and the
 * race has them (0x407370 with 1; the press itself only counts under 30
 * mph), so a key held from faster never starts them.  The page presses its
 * key again when this goes to 2. */
#define PL_MARKER    0x9acu
#define PL_CAR       4u
#define CAR_SPEED    0x430u           /* float: m/s */
#define G_OUTRUN_ST  0x00860a58u      /* Career4230 (0x85EF70) +0x1AE8 */
#define G_HYD_LEVEL  0x007e949cu      /* int: every car's hydraulics level, -1: each its own */
#define G_HYD_ON     0x007e94a0u      /* byte: the hydraulics at all */
int roam_game_engage(void); int roam_game_near(void);
static uint32_t rd32_ok(uint32_t a){ return a && mem_is_committed(a) && mem_is_committed(a + 3u) ? MR32(a) : 0u; }
static int hydraulics(void){
    uint32_t car = rd32_ok(rd32_ok(G_PLAYER0) + PL_CAR), ride = rd32_ok(car + 0x10u);
    int32_t lv = ride && mem_is_committed(ride + 0x581u) ? (int8_t)MR8(ride + 0x581u) : 0, all = (int32_t)rd32_ok(G_HYD_LEVEL);
    if ((all > -1 ? all : lv) == 0) return 0;
    /* 0x407370 (1): races of the kinds without them (the race parameters' bytes, as it reads them), the speed */
    if (!MR8(G_HYD_ON) || MR8(0x0089e7d9u) || MR32(0x00890104u) || MR8(0x0089e7e3u) || MR32(0x0089010cu)) return 1;
    return mem_is_committed(car + CAR_SPEED + 3u) && MRF32(car + CAR_SPEED) * 2.237f <= 10.f ? 2 : 1;
}
static void touch_state(int drive){
    static int slot[3] = { -2, -2, -2 };
    static double last[3] = { -1, -1, -1 };
    if (slot[0] == -2){ slot[0] = host_status_slot("engage"); slot[1] = host_status_slot("roam_near"); slot[2] = host_status_slot("hydraulics"); }
    int roam = (drive & 3) == 2;
    uint32_t p = roam && mem_is_committed(G_PLAYER0 + 3u) ? MR32(G_PLAYER0) : 0u;
    int marker = p && mem_is_committed(p + PL_MARKER + 3u) && MR32(p + PL_MARKER);
    int outrun = roam && mem_is_committed(G_OUTRUN_ST + 3u) && MR32(G_OUTRUN_ST) == 2u;
    double v[3] = { roam && (marker || outrun || roam_game_engage()), roam ? (double)roam_game_near() : 0, (drive & 3) ? hydraulics() : 0 };
    if (v[0] != last[0] || v[1] != last[1] || v[2] != last[2]){ last[0] = v[0]; last[1] = v[1]; last[2] = v[2]; host_status_set_n(slot, v, 3); }
}
/* "sms": the text message of ours (sms.c) the game has in its inbox this
 * game, 0 none, so the page asks for it no more (web/src/app/playtime.ts) */
int sms_delivered(void);
/* "nocount": 1 while a countdown of the player's would not start where they
 * are in free roam (roam_game_nocount: a car meet, and they have not stayed),
 * so quick chat's picker dims it (web/src/app/quickchat.ts) */
int roam_game_nocount(void);
void status_web_frame(void){
    static int slot = -2, last = -1, rslot = -2, rlast = -1, pslot = -2, plast = -1, sslot = -2, slast = -1, cslot = -2, clast = -1;
    if (slot == -2){ slot = host_status_slot("drive"); rslot = host_status_slot("rate"); pslot = host_status_slot("pause"); sslot = host_status_slot("sms");
                     cslot = host_status_slot("nocount"); }
    int v = drive_state();
    if (v != last && slot >= 0){ last = v; host_status_set(slot, v); }
    int r = hfr_rate_now();
    if (r != rlast && rslot >= 0){ rlast = r; host_status_set(rslot, r); }
    int p = pause_state();
    if (p != plast && pslot >= 0){ plast = p; host_status_set(pslot, p); }
    int m = sms_delivered();
    if (m != slast && sslot >= 0){ slast = m; host_status_set(sslot, m); }
    int nc = (v & 3) == 2 && roam_game_nocount();
    if (nc != clast && cslot >= 0){ clast = nc; host_status_set(cslot, nc); }
    roam_state();
    touch_state(v);
}

/* main_web.c, before the guest starts */
void status_web_start(void){
    g_st.magic = HOST_STATUS_MAGIC;
    g_st.radio.radio_magic = RADIO_MAGIC;
    g_st.radio.radio_version = RADIO_VERSION;
    g_st.period_ms = 250;
    nfsu2_register(HOST_KIND_STATUS, &g_st, sizeof g_st);
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u * 1024u);
    pthread_t th;
    if (pthread_create(&th, &at, status_main, 0) == 0) pthread_detach(th);
}
