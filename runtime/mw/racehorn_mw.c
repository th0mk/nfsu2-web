/* racehorn_mw.c -- horns in Most Wanted's races between players (its LAN and Online races), as runtime/racehorn.c
 * does them for Underground 2.  The sound is horn.c's synthesised horns: none of Most Wanted's 301 sound banks holds
 * a horn (TRACKER M8-17: the engine, nitrous, shifting, skid, turbo, global and front-end banks were decoded), so the
 * player's horn is one of horn.c's eight kinds (NFSU2_HORN), as in Underground 2.
 *
 * The key: N, which no row of the game's binding table uses (the defaults at 0x8F6D80: the letters in no row are
 * E F H J K N Q U Y), as in Underground 2.  Not the controller's left stick click (Underground 2's pad horn): the
 * game picks a joystick's driving bindings by its product name (0x63CD80), and those layouts are not read yet.
 *
 * The horn goes beside the game's race link (UDP 3658, a star around the game's creator), between the same games, on
 * the runtime's own UDP 3659 (net_lan.c's vlan_rt_*), so the game never sees it:
 *   - to whom: the games the race link exchanged datagrams with in the last PEER_MS (vlan_race_peers): every joiner
 *     for the creator, the creator for a joiner.  The creator passes each joiner's horn on to the other joiners,
 *     marked relayed (never passed on again), at most RELAY_PER_S of each joiner's a second (vlan_race_relay).
 *     A horn from an address that is not one of the race's peers is not heard and not passed on; so a joiner, whose
 *     one peer is the creator, passes nothing on.
 *   - what: "N2HN", version 2, flags (1 the horn sounds, 2 relayed, bits 2-5 which horn: horn.h's kinds), a sequence
 *     number, the player's name (its pitch, one of horn.c's five, and the log's), then where the player's car is and
 *     how it moves (the world frame of roam_mw.c: x = sim z, y = -sim x, z = sim y, metres and m/s, little-endian
 *     floats).  Underground 2's racehorn.c takes version 1 only and finds the car by the name in the game's player
 *     table; Most Wanted's table of the race's players is not known, so the message says where the car is, and the
 *     horn sounds from the race car nearest to that place (within SNAP_M), else from the place itself.  While the
 *     horn sounds, every SEND_MS; when it stops, twice.  A horn not heard of for STALE_MS stops.
 *   - where it is heard from: the listener is the player's own car (its place, forward and velocity).
 * In a race only: GameFlow 6, the player's car there, the race link busy (a peer within PEER_MS) and the simulation
 * stepping (not paused, not loading).  Only in the browser, where the virtual LAN is (net_lan.c; weak stand-ins
 * otherwise). */
#include "cpu.h"
#include "horn.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define G_FLOW       0x00925e90u   /* GameFlow: 6 in the world */
#define G_SIM        0x009203b0u   /* the simulation: +0x28 the steps it ran */
#define G_SIMLIST    0x0090d664u   /* every vehicle's ISimable (the racers first): +0 the array, +8 the count */
#define G_ONLINE     0x0091cf90u   /* the online state object: +0x12C its mode, +0x134 the login record L */
#define ON_LOGIN     0x134u
#define L_LAN_NAME   0xacu         /* LAN's account and persona (L+0xAC, +0xC0), Online's persona L+0x84: 16 characters */
#define L_LAN_PERS   0xc0u
#define L_ON_PERS    0x84u
#define SIM_RB       0x54u         /* ISimable::GetRigidBody; the body's data at [[rb + 0x30]] */
#define RBD_POS      0x10u
#define RBD_VEL      0x20u
#define RBD_FWD      0x90u
#define HORN_PORT    3659u         /* next to the race link's 3658; the game binds neither else */
#define PEER_MS      5000u
#define SEND_MS      100u
#define RELAY_PER_S  30u           /* a joiner's horn messages passed on a second: a horn sends one every SEND_MS,
                                    * and a player tapping N up to about 3 a tap (on, off, off) */
#define STALE_MS     300u
#define RUN_MS       500u          /* the simulation stepped this recently: the race runs */
#define SNAP_M       30.f
#define NAME_N       16
#define MSG_N        (8 + NAME_N + 24)
#define NHORNS       8

uint32_t mw_aid_call(CPU *c, uint32_t fn, uint32_t self, int n, const uint32_t *args);   /* career_mw.c */
uint32_t mw_aid_simable(CPU *c);
/* net_lan.c in the browser */
__attribute__((weak)) int vlan_rt_send(uint16_t port, uint32_t ip_h, const void *p, int n){ (void)port; (void)ip_h; (void)p; (void)n; return -1; }
__attribute__((weak)) int vlan_rt_recv(uint16_t port, uint32_t *ip_h, void *p, int cap){ (void)port; (void)ip_h; (void)p; (void)cap; return -1; }
__attribute__((weak)) int vlan_race_peers(uint32_t *ip_h, int max, uint32_t within_ms){ (void)ip_h; (void)max; (void)within_ms; return 0; }
__attribute__((weak)) int vlan_race_relay(uint32_t ip_h, uint32_t within_ms, uint32_t per_s){ (void)ip_h; (void)within_ms; (void)per_s; return -1; }

static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n - 1u); }
static uint32_t rd(uint32_t a){ return ok(a, 4) ? MR32(a) : 0u; }
static float rdf(uint32_t a){ uint32_t v = rd(a); float f; memcpy(&f, &v, 4); return f; }
static uint32_t now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}
static uint32_t fnv1a(const char *s){
    uint32_t h = 2166136261u;
    for (; *s; s++){ h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}
static void s2w(const float *s, float *w){ float x = s[0], y = s[1], z = s[2]; w[0] = z; w[1] = -x; w[2] = y; }
static void put_f(uint8_t *m, float f){ uint32_t v; memcpy(&v, &f, 4); m[0] = (uint8_t)v; m[1] = (uint8_t)(v >> 8); m[2] = (uint8_t)(v >> 16); m[3] = (uint8_t)(v >> 24); }
static float get_f(const uint8_t *m){ uint32_t v = m[0] | (uint32_t)m[1] << 8 | (uint32_t)m[2] << 16 | (uint32_t)m[3] << 24; float f; memcpy(&f, &v, 4); return f; }

/* a car's place, forward and velocity in the world frame; 0 when it has no rigid body */
static int car_state(CPU *c, uint32_t sim, float p[3], float f[3], float v[3]){
    uint32_t vt = rd(sim), fn = vt ? rd(vt + SIM_RB) : 0u;
    uint32_t rb = fn ? mw_aid_call(c, fn, sim, 0, 0) : 0u, d = rb ? rd(rd(rb + 0x30u)) : 0u;
    if (!ok(d, 0xa0)) return 0;
    float sp[3], sf[3], sv[3];
    for (int k = 0; k < 3; k++){ sp[k] = rdf(d + RBD_POS + 4u * (uint32_t)k); sf[k] = rdf(d + RBD_FWD + 4u * (uint32_t)k); sv[k] = rdf(d + RBD_VEL + 4u * (uint32_t)k); }
    for (int k = 0; k < 3; k++) if (!isfinite(sp[k]) || !isfinite(sf[k]) || !isfinite(sv[k])) return 0;
    s2w(sp, p); if (f) s2w(sf, f); if (v) s2w(sv, v);
    return 1;
}
/* the name the player races under: LAN's persona (or account), Online's persona; printable, at most 15 */
static void my_name(char *o){
    uint32_t on = rd(G_ONLINE), L = on ? on + ON_LOGIN : 0u;
    static const uint32_t at[3] = { L_LAN_PERS, L_LAN_NAME, L_ON_PERS };
    o[0] = 0;
    for (int j = 0; L && j < 3 && !o[0]; j++){
        uint32_t a = L + at[j];
        if (!ok(a, NAME_N)) continue;
        int n = 0;
        for (; n < NAME_N - 1; n++){ uint8_t ch = MR8(a + (uint32_t)n); if (ch < 0x20 || ch > 0x7e) break; o[n] = (char)ch; }
        o[n] = 0;
        if (MR8(a + (uint32_t)n)) o[0] = 0;         /* not a whole name */
    }
}

static struct { char name[NAME_N]; uint32_t at; int on, kind; float p[3], v[3]; } g_h[NHORNS];   /* others' horns as last heard */
static uint16_t g_seq;
static int g_mine, g_offs, g_active;
static uint32_t g_sent_at, g_steps, g_stepped_at;

static void send_all(const uint8_t *m, uint32_t except){
    uint32_t peers[16];
    int n = vlan_race_peers(peers, 16, PEER_MS);
    for (int i = 0; i < n; i++) if (peers[i] != except) vlan_rt_send(HORN_PORT, peers[i], m, MSG_N);
}
static void got(uint8_t *m, int n, uint32_t from, uint32_t now){
    if (n < MSG_N || memcmp(m, "N2HN", 4) || m[4] != 2) return;
    char name[NAME_N];
    int k = 0;
    for (; k < NAME_N - 1 && m[8 + k] >= 0x20 && m[8 + k] <= 0x7e; k++) name[k] = (char)m[8 + k];
    name[k] = 0;
    if (!name[0]) return;
    int pass = !(m[5] & 2), r = vlan_race_relay(from, PEER_MS, pass ? RELAY_PER_S : 0u);
    if (r < 0) return;                                  /* not one of the race's games */
    if (pass && r){ m[5] |= 2; send_all(m, from); }     /* the creator passes it on */
    int j = 0;
    while (j < NHORNS && strcmp(g_h[j].name, name)) j++;
    if (j == NHORNS){
        for (j = 0; j < NHORNS && g_h[j].name[0] && g_h[j].on && now - g_h[j].at < STALE_MS; j++) {}
        if (j == NHORNS) return;
        memcpy(g_h[j].name, name, NAME_N);
    }
    g_h[j].on = m[5] & 1; g_h[j].kind = m[5] >> 2 & 15; g_h[j].at = now;
    for (int a = 0; a < 3; a++){ g_h[j].p[a] = get_f(m + 8 + NAME_N + 4 * a); g_h[j].v[a] = get_f(m + 20 + NAME_N + 4 * a); }
}

int win32_reg_get_dword(const char *nm, uint32_t *out);   /* win32_ext.c */
/* every pass of the main loop, on the game's thread (game_mw.c on_frame) */
void racehorn_mw_frame(CPU *c){
    static int vol_read;
    if (!vol_read){                                      /* the horns' volume: the registry's HornVolume (percent), as
                                                          * Underground 2's Horn Volume slider keeps it (hornmenu.c) */
        uint32_t pct;
        vol_read = 1;
        if (win32_reg_get_dword("HornVolume", &pct)){
            pct = pct < 10u ? 10u : pct > 100u ? 100u : pct;
            horn_set_volume((float)pct / 100.f);
            fprintf(stderr, "[horn] volume %u %% (registry HornVolume)\n", pct);
        }
    }
    uint32_t now = now_ms(), from;
    uint8_t m[256];
    int n;
    while ((n = vlan_rt_recv(HORN_PORT, &from, m, sizeof m)) >= 0) got(m, n, from, now);
    uint32_t sim = rd(G_SIM), steps = sim ? rd(sim + 0x28u) : 0u;
    if (steps != g_steps){ g_steps = steps; g_stepped_at = now; }
    uint32_t peer;
    int racing = rd(G_FLOW) == 6u && now - g_stepped_at < RUN_MS && vlan_race_peers(&peer, 1, PEER_MS) > 0;
    char mine[NAME_N];
    my_name(mine);
    int key = racing && mine[0] && horn_held(0), heard = 0;
    for (int j = 0; j < NHORNS && !heard; j++) heard = g_h[j].on && now - g_h[j].at < STALE_MS;
    if (!key && !g_mine && !g_offs && !g_active && !(racing && heard)) return;   /* no horn: no guest calls */
    uint32_t me = racing ? mw_aid_simable(c) : 0u;
    float L[3] = {0}, F[3] = {1.f, 0.f, 0.f}, V[3] = {0};
    int runs = me && car_state(c, me, L, F, V);
    /* our horn to the others */
    int horn = runs && key;
    if (horn != g_mine){ g_mine = horn; g_offs = horn ? 0 : 2; g_sent_at = now - SEND_MS; }
    if (mine[0] && (g_mine || g_offs) && now - g_sent_at >= SEND_MS){
        memset(m, 0, MSG_N);
        memcpy(m, "N2HN", 4); m[4] = 2; m[5] = (uint8_t)(g_mine | (horn_mine() & 15) << 2); m[6] = (uint8_t)g_seq; m[7] = (uint8_t)(g_seq >> 8); g_seq++;
        memcpy(m + 8, mine, strlen(mine));
        for (int a = 0; a < 3; a++){ put_f(m + 8 + NAME_N + 4 * a, L[a]); put_f(m + 20 + NAME_N + 4 * a, V[a]); }
        send_all(m, 0);
        g_sent_at = now;
        if (!g_mine) g_offs--;
    }
    /* the horns heard here */
    if (!runs && !g_active) return;                      /* free roam may have the voices */
    horn_begin(runs ? L : 0, F, V);
    if (horn) horn_sound(fnv1a(mine), mine, horn_mine(), 0, 0);
    uint32_t sb = rd(G_SIMLIST), sn = rd(G_SIMLIST + 8u);
    if (sn > 8u) sn = 8u;                                /* the racers come first: 4 players at most */
    for (int j = 0; j < NHORNS; j++){
        if (!runs || !g_h[j].on || now - g_h[j].at >= STALE_MS) continue;
        float age = (float)(now - g_h[j].at) / 1000.f, p[3], v[3], best = SNAP_M * SNAP_M;
        for (int a = 0; a < 3; a++){ p[a] = g_h[j].p[a] + g_h[j].v[a] * age; v[a] = g_h[j].v[a]; }
        float q[3] = { p[0], p[1], p[2] };
        for (uint32_t i = 0; sb && i < sn; i++){         /* the race car nearest to where the message puts it */
            uint32_t s = rd(sb + 4u * i);
            float cp[3], cv[3];
            if (!s || s == me || !car_state(c, s, cp, 0, cv)) continue;
            float d2 = (cp[0] - q[0]) * (cp[0] - q[0]) + (cp[1] - q[1]) * (cp[1] - q[1]) + (cp[2] - q[2]) * (cp[2] - q[2]);
            if (d2 < best){ best = d2; memcpy(p, cp, sizeof p); memcpy(v, cv, sizeof v); }
        }
        horn_sound(fnv1a(g_h[j].name), g_h[j].name, g_h[j].kind, p, v);
    }
    horn_end();
    static int nlog;
    if (runs && !g_active && ++nlog <= 10)               /* a race starts (or goes on) */
        fprintf(stderr, "[horn] race: me %s, %u cars, horn %s (key N)\n", mine[0] ? mine : "(no name)",
                sn, horn_name(horn_mine()));
    g_active = runs;
}
