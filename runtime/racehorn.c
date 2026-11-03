/* racehorn.c -- horns in races between players (the game's Online and LAN
 * races).  Free roam has its own (roam_game.c); the sound is horn.c's.
 *
 * In a network race the other players' cars are the game's own: its race
 * link moves them with datagrams on UDP 3658 between the players' games, a
 * star around the game's creator (web/server-rs/crates/online).  The horn goes beside
 * that link, between the same games, on a UDP port of the runtime's own
 * (HORN_PORT, net_lan.c's vlan_rt_*), so the game never sees it:
 *   - to whom: the games the race link exchanged datagrams with in the last
 *     PEER_MS (vlan_race_peers): every joiner for the creator, the creator
 *     for a joiner.  The creator passes each joiner's horn on to the other
 *     joiners, marked relayed (never passed on again).
 *   - what: "N2HN", version 1, flags (1 the horn sounds, 2 relayed, bits
 *     2-5 which horn: horn.h's kinds, which a build before them leaves at 0,
 *     the dual one, and ignores), a sequence number, and the name the
 *     player races under, as the game shows it over their car on every
 *     screen: the network race's player
 *     table (G_NETPLAYERS: player i's name at +0x12, which its own lookup by
 *     name 0x5F0230 compares; the HUD's names over the cars, 0x4EC0D0, pair
 *     player i with the world's car whose +0xA is i, World 0x5F0E30).  While
 *     the horn sounds, every SEND_MS; when it stops, twice.  A horn not heard
 *     of for STALE_MS stops.
 *   - where it sounds: from the car of the player with that name.  (A car's
 *     racer record names the profile, not the player: "No Profile" for a
 *     player without one, nothing for the others' cars.)
 * The port also carries the players' frame rates ("N2FS"), the joiners' car
 * states straight from their games ("N2CS") and the creator's list of the
 * joiners they go to ("N2PL"), which go to racenet.c, a racer's Quit
 * ("N2QT", which goes to mprace.c) and quick chat ("N2QC", below: qc_*).
 * Only in the browser, where Online and the virtual LAN are; the Mac's LAN
 * has no such port (the weak stand-ins below). */
#include "cpu.h"
#include "horn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define G_FLOW       0x008654a4u   /* 6: in the world */
#define G_NETMODE    0x00850078u   /* 8 Online, 0x200 LAN (1 career) */
#define G_WORLD      0x00890080u   /* World*: +0 the cars' list (a car's first two dwords are its node) */
#define G_PLAYER1    0x008900acu   /* Player*: +4 car */
#define G_WORLDDT    0x00865154u   /* float: this frame's world dt (s) */
#define G_VIEW1      0x00832e50u   /* eView 1: +0x40 camera (+0x40 position, +0x50 forward) */
#define G_NETPLAYERS 0x0089cf50u   /* the network race's players: 6 pointers (0x89CF48 + 8), name at +0x12 */
#define G_HYD_LEVEL  0x007e949cu   /* int: every car's hydraulics level, -1: each its own (a debug setting) */
#define NP_NAME      0x12u
#define HORN_PORT    3659u         /* next to the race link's 3658; the game binds neither else */
#define PEER_MS      5000u
#define SEND_MS      100u
#define STALE_MS     300u
#define NAME_N       16
#define MSG_N        (8 + NAME_N)
#define NHORNS       8

/* net_lan.c in the browser; none on the Mac */
__attribute__((weak)) int vlan_rt_send(uint16_t port, uint32_t ip_h, const void *p, int n){ (void)port; (void)ip_h; (void)p; (void)n; return -1; }
__attribute__((weak)) int vlan_rt_recv(uint16_t port, uint32_t *ip_h, void *p, int cap){ (void)port; (void)ip_h; (void)p; (void)cap; return -1; }
__attribute__((weak)) int vlan_race_peers(uint32_t *ip_h, int max, uint32_t within_ms){ (void)ip_h; (void)max; (void)within_ms; return 0; }
__attribute__((weak)) int vlan_race_sent_to(uint32_t ip_h, uint32_t within_ms){ (void)ip_h; (void)within_ms; return 0; }
__attribute__((weak)) int vlan_rt_say_pick(void){ return -1; }
__attribute__((weak)) int vlan_rt_said(const char *json, int n){ (void)json; (void)n; return -1; }
/* status_web.c in the browser; the Mac has no page */
__attribute__((weak)) int host_status_slot(const char *name){ (void)name; return -1; }
__attribute__((weak)) void host_status_set_n(const int *slot, const double *value, int n){ (void)slot; (void)value; (void)n; }
const char *autotest_top_fng(void);

static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n); }
static uint32_t rd32(uint32_t a){ return ok(a, 3) ? MR32(a) : 0u; }
static void rd3(uint32_t a, float *o){ o[0] = MRF32(a); o[1] = MRF32(a + 4u); o[2] = MRF32(a + 8u); }
static uint32_t now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}
static uint32_t fnv1a(const char *s){
    uint32_t h = 2166136261u;
    for (; *s; s++){ h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}
/* the name of the player whose car this is (printable, at most 15
 * characters), "" for none */
static void car_name(uint32_t car, char *o){
    int i = ok(car, 0x9e0) ? (int16_t)MR16(car + 0xau) : -1;
    uint32_t pl = i >= 0 && i < 6 ? rd32(G_NETPLAYERS + 4u * (uint32_t)i) : 0;
    int n = 0;
    if (ok(pl, NP_NAME + NAME_N))
        for (; n < NAME_N - 1; n++){ uint8_t ch = MR8(pl + NP_NAME + (uint32_t)n); if (ch < 0x20 || ch > 0x7e) break; o[n] = (char)ch; }
    o[n] = 0;
}
/* the car has hydraulics, so the stick click is not the horn (horn.h; as
 * roam_game.c's has_hydraulics: RideInfo +0x581, or G_HYD_LEVEL's when not -1) */
static int has_hydraulics(uint32_t car){
    uint32_t ride = ok(car, 0x14) ? rd32(car + 0x10u) : 0;
    int32_t lv = ok(ride, 0x581) ? (int8_t)MR8(ride + 0x581u) : 0, ov = (int32_t)rd32(G_HYD_LEVEL);
    return (ov > -1 ? ov : lv) != 0;
}

static struct { char name[NAME_N]; uint32_t at; int on, kind; } g_h[NHORNS];   /* others' horns as last heard */
static uint16_t g_seq;
static int g_mine, g_offs, g_active;                 /* our horn; "stopped" messages still to send; voices driven */
static uint32_t g_sent_at;

/* to the race's other players (but except): how many it went to */
static int send_all(const uint8_t *m, uint32_t except){
    uint32_t peers[16];
    int n = vlan_race_peers(peers, 16, PEER_MS), to = 0;
    for (int i = 0; i < n; i++) if (peers[i] != except && vlan_rt_send(HORN_PORT, peers[i], m, MSG_N) == 0) to++;
    return to;
}

/* Quick chat in races between players (web/src/app/quickchat.ts; free roam has its own, roam_game.c say_*).  The
 * page opens its picker while the status "netrace" says so (qc_status: 1 driving in a network race, 2 on its
 * results), and puts the phrase picked in the LAN mailbox's say word (vlan_rt_say_pick).  It goes to the race's
 * players as the horn does: "N2QC", version 1, flags (2 relayed), the sender's sequence number, the phrase (its
 * number in phrases.ts: any number goes, the page shows those it has), the name the player races under; the creator
 * passes each joiner's on to the other joiners.  Free roam's limits: at most QC_BURST phrases in QC_WINDOW_MS from one
 * player (the player's own too), and the same phrase from one player again within QC_AGAIN_MS is not shown; the
 * creator holds back what it would not show, so every player drops the same ones.  What is shown goes to the page
 * (NET_KIND_SAY, vlan_rt_said) as {"id","name","p","race":1} (+ "me" for the player's own), which leaves out the
 * players it blocks (settings.ini roam_block).  quick_chat off (NFSU2_ROAM_SAY=0): nothing said and nothing shown (a
 * creator still passes the others' on).  A build before N2QC drops it (it is no N2HN), so a creator on one passes
 * no joiner's phrase on; a joiner on one only misses them. */
#define QC_BURST     3
#define QC_WINDOW_MS 6000u
#define QC_AGAIN_MS  2000u
#define QC_WHO       8
static struct { char name[NAME_N]; uint32_t t[QC_BURST], at, heard_at; uint8_t seq, phrase, heard; } g_qc[QC_WHO];
static uint32_t g_qc_mine[QC_BURST];
static uint8_t g_qc_seq;
static int g_qc_test = -1;                          /* a script's "roam racesay N" (roam.c; -1 none) */
static int g_qc_racing;                             /* this frame: in a network race (racehorn_frame's racing) */
static char g_qc_me[NAME_N];                        /*   and the name we race under */
static int qc_on(void){
    static int on = -1;
    if (on < 0){ const char *v = getenv("NFSU2_ROAM_SAY"); on = !(v && (v[0] == '0' || !strcasecmp(v, "off"))); }
    return on;
}
static int qc_burst(const uint32_t *t, uint32_t now){
    int k = 0;
    for (int i = 0; i < QC_BURST; i++) k += t[i] && now - t[i] < QC_WINDOW_MS;
    return k >= QC_BURST;
}
static void qc_stamp(uint32_t *t, uint32_t now){
    int o = 0;
    for (int i = 1; i < QC_BURST; i++) if (t[i] < t[o]) o = i;    /* the oldest goes */
    t[o] = now ? now : 1u;
}
static void qc_page(const char *name, int phrase, int me){
    char js[128];
    int o = snprintf(js, sizeof js, "{\"id\":%u,\"name\":\"", fnv1a(name));
    for (const char *s = name; *s && o < (int)sizeof js - 40; s++){ if (*s == '"' || *s == '\\') js[o++] = '\\'; js[o++] = *s; }
    o += snprintf(js + o, sizeof js - (size_t)o, "\",\"p\":%d,\"race\":1%s}", phrase, me ? ",\"me\":1" : "");
    if (vlan_rt_said(js, o) < 0) fprintf(stderr, "[racechat] the page's ring is full: a line lost\n");
}
/* the player's pick: to the others, and to the page as said */
static void qc_say(int phrase, uint32_t now){
    if (!qc_on()){ fprintf(stderr, "[racechat] phrase %d not said (quick chat is off)\n", phrase); return; }
    if (!g_qc_racing || !g_qc_me[0]){ fprintf(stderr, "[racechat] phrase %d not said (not in a race between players)\n", phrase); return; }
    if (qc_burst(g_qc_mine, now)){ fprintf(stderr, "[racechat] phrase %d not said (too many at once)\n", phrase); return; }
    qc_stamp(g_qc_mine, now);
    uint8_t m[MSG_N];
    memset(m, 0, sizeof m);
    memcpy(m, "N2QC", 4); m[4] = 1; m[6] = ++g_qc_seq; m[7] = (uint8_t)phrase;
    memcpy(m + 8, g_qc_me, strlen(g_qc_me));
    int to = send_all(m, 0);
    qc_page(g_qc_me, phrase, 1);
    fprintf(stderr, "[racechat] %s said phrase %d to %d player(s)\n", g_qc_me, phrase, to);
}
/* another player's, from them or passed on by the creator */
static void qc_got(uint8_t *m, uint32_t from, uint32_t now){
    char name[NAME_N];
    int k = 0;
    for (; k < NAME_N - 1 && m[8 + k] >= 0x20 && m[8 + k] <= 0x7e; k++) name[k] = (char)m[8 + k];
    name[k] = 0;
    if (!name[0] || !strcmp(name, g_qc_me)) return;
    int j = 0, o = 0;
    while (j < QC_WHO && strcmp(g_qc[j].name, name)) j++;
    if (j == QC_WHO){                                     /* a new one takes the place heard from longest ago */
        for (j = 1; j < QC_WHO; j++) if (g_qc[j].heard_at < g_qc[o].heard_at) o = j;
        j = o;
        memset(&g_qc[j], 0, sizeof g_qc[j]);
        memcpy(g_qc[j].name, name, NAME_N);
    }
    int phrase = m[7], relayed = m[5] & 2;
    const char *why = 0;
    if (g_qc[j].heard && m[6] == g_qc[j].seq) why = "again";                     /* the same one twice */
    else if (qc_burst(g_qc[j].t, now)) why = "too many at once";
    else if (g_qc[j].at && phrase == g_qc[j].phrase && now - g_qc[j].at < QC_AGAIN_MS) why = "the same again";
    g_qc[j].seq = m[6]; g_qc[j].heard = 1; g_qc[j].heard_at = now;
    if (why){ fprintf(stderr, "[racechat] %s's phrase %d not shown (%s)\n", name, phrase, why); return; }
    qc_stamp(g_qc[j].t, now);
    g_qc[j].phrase = (uint8_t)phrase; g_qc[j].at = now;
    int to = 0;
    if (!relayed){ m[5] |= 2; to = send_all(m, from); }  /* the creator passes it on */
    if (!qc_on() || !g_qc_racing){
        fprintf(stderr, "[racechat] %s's phrase %d not shown (%s)%s\n", name, phrase, qc_on() ? "not in a race between players" : "quick chat is off",
                to ? ", passed on" : "");
        return;
    }
    qc_page(name, phrase, 0);
    fprintf(stderr, "[racechat] %s said phrase %d (%s)%s\n", name, phrase, relayed ? "passed on by the race's creator" : "straight", to ? ", passed on" : "");
}
/* roam.c's "roam racesay N": the phrase as if the player picked it on the page */
void racehorn_test_say(int phrase){ g_qc_test = phrase; }
/* "netrace" for the page (status_web.c's status block; drift.c's way): 1 while the player drives in a race between
 * players (a race HUD the top package, the start's cutscene too), 2 on its results (UI_PostRace*), 0 elsewhere (the
 * pause menu, the loading screen, a race of the career or a Quick Race: not the network mode).  Not one of "drive"'s
 * bits: that is 0 on the results, and more than 0 brings the touch controls' driving buttons */
static void qc_status(void){
    static int slot = -2, last = -1;
    if (slot == -2) slot = host_status_slot("netrace");
    const char *top = g_qc_racing && g_qc_me[0] ? autotest_top_fng() : 0;
    int v = !top ? 0 : !strncasecmp(top, "HUD_", 4) && strcasecmp(top, "HUD_World_Loading.fng") ? 1
          : !strncasecmp(top, "UI_PostRace", 11) ? 2 : 0;
    if (v == last || slot < 0) return;
    last = v;
    double d = v;
    host_status_set_n(&slot, &d, 1);
    fprintf(stderr, "[racechat] netrace %d (%s)\n", v, top ? top : "-");
}

void racenet_heard(uint8_t *m, int n, uint32_t from);   /* racenet.c: the players' frame rates, */
void racenet_carspam(CPU *c, const uint8_t *m, int n, uint32_t from);   /*   a joiner's car state straight from its game, */
void racenet_list(const uint8_t *m, int n, uint32_t from);              /*   and the creator's list of the joiners */
void mprace_quit_heard(uint8_t *m, int n, uint32_t from);              /* mprace.c: a racer that quit the race */
static void got(CPU *c, uint8_t *m, int n, uint32_t from, uint32_t now){
    if (n >= 4 && !memcmp(m, "N2FS", 4)){ racenet_heard(m, n, from); return; }
    if (n >= 4 && !memcmp(m, "N2CS", 4)){ racenet_carspam(c, m, n, from); return; }
    if (n >= 4 && !memcmp(m, "N2PL", 4)){ racenet_list(m, n, from); return; }
    if (n >= 4 && !memcmp(m, "N2QT", 4)){ mprace_quit_heard(m, n, from); return; }
    if (n >= MSG_N && !memcmp(m, "N2QC", 4)){ if (m[4] == 1) qc_got(m, from, now); return; }
    if (n < MSG_N || memcmp(m, "N2HN", 4) || m[4] != 1) return;
    char name[NAME_N];
    int k = 0;
    for (; k < NAME_N - 1 && m[8 + k] >= 0x20 && m[8 + k] <= 0x7e; k++) name[k] = (char)m[8 + k];
    name[k] = 0;
    if (!name[0]) return;
    if (!(m[5] & 2)){ m[5] |= 2; send_all(m, from); }   /* the creator passes it on */
    int j = 0;
    while (j < NHORNS && strcmp(g_h[j].name, name)) j++;
    if (j == NHORNS){
        for (j = 0; j < NHORNS && g_h[j].name[0] && g_h[j].on && now - g_h[j].at < STALE_MS; j++) {}
        if (j == NHORNS) return;
        memcpy(g_h[j].name, name, NAME_N);
    }
    g_h[j].on = m[5] & 1; g_h[j].kind = m[5] >> 2 & 15; g_h[j].at = now;
}

/* every frame, on the game thread (roam.c's main-loop hook) */
void racehorn_frame(CPU *c){
    uint32_t now = now_ms(), from;
    static uint8_t m[8 + 0x400];                         /* (a car state: racenet.c's N2CS, up to a bit stream's 0x400 bytes) */
    int n;
    uint32_t pl = rd32(G_PLAYER1), me = pl ? rd32(pl + 4u) : 0, w = rd32(G_WORLD);
    float dt = MRF32(G_WORLDDT);
    int racing = rd32(G_FLOW) == 6u && (rd32(G_NETMODE) & 0x208u) && ok(me, 0x9e0) && ok(w, 0x80);
    int runs = racing && dt > 0.f && dt < 1.f;           /* not paused, not loading */
    char mine[NAME_N];
    car_name(me, mine);
    g_qc_racing = racing; memcpy(g_qc_me, racing ? mine : "", racing ? NAME_N : 1);
    while ((n = vlan_rt_recv(HORN_PORT, &from, m, sizeof m)) >= 0) got(c, m, n, from, now);
    /* quick chat: the player's pick (taken in any case, so none waits for the next race) */
    int pick = vlan_rt_say_pick();
    if (g_qc_test >= 0){ pick = g_qc_test; g_qc_test = -1; }
    if (pick >= 0) qc_say(pick, now);
    qc_status();
    /* our horn to the others */
    int horn = runs && mine[0] && horn_held(!has_hydraulics(me));
    if (horn != g_mine){ g_mine = horn; g_offs = horn ? 0 : 2; g_sent_at = now - SEND_MS; }
    if (mine[0] && (g_mine || g_offs) && now - g_sent_at >= SEND_MS){
        memset(m, 0, MSG_N);
        memcpy(m, "N2HN", 4); m[4] = 1; m[5] = (uint8_t)(g_mine | (horn_mine() & 15) << 2); m[6] = (uint8_t)g_seq; m[7] = (uint8_t)(g_seq >> 8); g_seq++;
        memcpy(m + 8, mine, strlen(mine));
        send_all(m, 0);
        g_sent_at = now;
        if (!g_mine) g_offs--;
    }
    /* the horns heard here */
    if (!runs && !g_active) return;                      /* free roam may have the voices */
    float L[3] = {0}, f[3] = {1.f, 0.f, 0.f}, v[3] = {0};
    uint32_t cam = rd32(G_VIEW1 + 0x40u);
    if (runs && ok(cam, 0x60)){ rd3(cam + 0x40u, L); rd3(cam + 0x50u, f); }
    else if (runs){ rd3(me + 0x60u, L); rd3(me + 0x90u, f); }
    if (runs) rd3(me + 0x70u, v);
    horn_begin(runs ? L : 0, f, v);
    if (horn) horn_sound(fnv1a(mine), mine, horn_mine(), 0, 0);
    for (int j = 0; j < NHORNS; j++){
        if (!runs || !g_h[j].on || now - g_h[j].at >= STALE_MS) continue;
        uint32_t car = rd32(w);
        for (int k = 0; car && car != w && k < 64 && ok(car, 0x9e0); k++, car = rd32(car)){
            char nm[NAME_N];
            if (car == me) continue;
            car_name(car, nm);
            if (strcmp(nm, g_h[j].name)) continue;
            float p[3], cv[3];
            rd3(car + 0x60u, p); rd3(car + 0x70u, cv);
            horn_sound(fnv1a(nm), nm, g_h[j].kind, p, cv);
            break;
        }
    }
    horn_end();
    static int nlog;
    if (runs && !g_active && ++nlog <= 10){              /* a race starts (or goes on): who is in it, by name */
        char line[200]; int o = snprintf(line, sizeof line, "[horn] race: me %s, cars", mine[0] ? mine : "(no name)");
        uint32_t car = rd32(w);
        for (int k = 0; car && car != w && k < 64 && ok(car, 0x9e0) && o < (int)sizeof line - 20; k++, car = rd32(car)){
            char nm[NAME_N]; car_name(car, nm);
            o += snprintf(line + o, sizeof line - (size_t)o, " %s", nm[0] ? nm : "-");
        }
        fprintf(stderr, "%s\n", line);
    }
    g_active = runs;
}
