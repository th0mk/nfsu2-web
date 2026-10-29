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
 * joiners they go to ("N2PL"), which go to racenet.c.
 * Only in the browser, where Online and the virtual LAN are; the Mac's LAN
 * has no such port (the weak stand-ins below). */
#include "cpu.h"
#include "horn.h"
#include <stdio.h>
#include <string.h>
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

static void send_all(const uint8_t *m, uint32_t except){
    uint32_t peers[16];
    int n = vlan_race_peers(peers, 16, PEER_MS);
    for (int i = 0; i < n; i++) if (peers[i] != except) vlan_rt_send(HORN_PORT, peers[i], m, MSG_N);
}
void racenet_heard(uint8_t *m, int n, uint32_t from);   /* racenet.c: the players' frame rates, */
void racenet_carspam(CPU *c, const uint8_t *m, int n, uint32_t from);   /*   a joiner's car state straight from its game, */
void racenet_list(const uint8_t *m, int n, uint32_t from);              /*   and the creator's list of the joiners */
static void got(CPU *c, uint8_t *m, int n, uint32_t from, uint32_t now){
    if (n >= 4 && !memcmp(m, "N2FS", 4)){ racenet_heard(m, n, from); return; }
    if (n >= 4 && !memcmp(m, "N2CS", 4)){ racenet_carspam(c, m, n, from); return; }
    if (n >= 4 && !memcmp(m, "N2PL", 4)){ racenet_list(m, n, from); return; }
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
    while ((n = vlan_rt_recv(HORN_PORT, &from, m, sizeof m)) >= 0) got(c, m, n, from, now);
    uint32_t pl = rd32(G_PLAYER1), me = pl ? rd32(pl + 4u) : 0, w = rd32(G_WORLD);
    float dt = MRF32(G_WORLDDT);
    int racing = rd32(G_FLOW) == 6u && (rd32(G_NETMODE) & 0x208u) && ok(me, 0x9e0) && ok(w, 0x80);
    int runs = racing && dt > 0.f && dt < 1.f;           /* not paused, not loading */
    char mine[NAME_N];
    car_name(me, mine);
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
