/* roam_web.c -- free-roam multiplayer: the browser's platform layer (wasm),
 * in place of roam_bsd.c.  See roam_plat.h.
 *
 * A page cannot open sockets.  The page's main thread holds one WebSocket to
 * the local server's virtual-LAN switch (web/server-rs/crates/lan, /lan), which gives
 * every browser a virtual IPv4 address (10.77.x.x) and routes frames by virtual
 * destination address.  This file is one virtual UDP endpoint on that LAN:
 *
 *   game thread / network thread --TX ring--> page (web/src/net) --WebSocket--> switch
 *   network thread <--RX ring-- page <--WebSocket-- switch
 *
 * The rings live in the net mailbox (net_proto.h) in shared wasm memory,
 * registered with nfsu2_register(HOST_KIND_NET).  The page writes our virtual
 * address into it once the switch has assigned one.  The network thread
 * (a pthread, i.e. a worker of the page's pool) blocks on the RX ring's bell
 * with memory.atomic.wait32 and a timeout, feeds every datagram to the session
 * and runs rc_tick every 10 ms, all under roam_plat_lock(), as roam_bsd.c's
 * thread does with poll() and recvfrom().  Sending (from either thread, with
 * the lock held) queues a datagram on the TX ring and rings the page.
 *
 * Addresses (what the core sees):
 *   player   our own virtual address and port; peers are virtual addresses
 *            (roam_peers takes 10.77.x.x literals, the page shows them).  Who
 *            our peers are comes from the server's free-roam hub
 *            (NET_ROAM_HUB, web/server-rs/crates/roam: the players nearest to
 *            us, rc_config.hub_ip), so a server takes far more players than
 *            one group; NFSU2_ROAM_HUB=0 does without it.  Without a hub
 *            answering, discovery (roam_discovery = on) is one datagram to
 *            the switch's broadcast address, i.e. every other browser on the
 *            server.  A destination outside the virtual subnet cannot be
 *            reached: a send error, logged.
 *   test     (roam_plat_test_mode(): an automated run, the same rule as the
 *            Mac) the Mac's guard is kept word for word: only 127/8
 *            destinations, no broadcast, peers only 127/8, every refusal
 *            logged as "[roam] BLOCKED".  What 127.0.0.1 means is adapted: the
 *            switch stands for the Mac's loopback interface.  A datagram to
 *            127.x.x.x:P goes out as a NET_KIND_UDP_LOOP frame to the switch's
 *            broadcast address; only instances in test mode bound to port P
 *            take it, and they see it come from 127.0.0.1:<sender's port>.  So
 *            the Mac's test setup works unchanged (every instance its own
 *            roam_port, peers 127.0.0.1:<port>), and test traffic and players'
 *            traffic never meet, even on one switch: a player's endpoint drops
 *            loopback frames, a test endpoint drops player frames.  As on the
 *            Mac, two test instances must not share a port (there is no bind
 *            to fail here: both would take the datagrams).
 *   NFSU2_ROAM_WEBTEST=vlan
 *            (web only, a test aid) an automated run that uses the virtual LAN
 *            as a player does (virtual addresses, discovery): the two-browser
 *            test of the players' path.  Allowed because nothing here reaches
 *            a real network: the switch forwards only to browsers attached to
 *            the same server.
 *
 * A SNAP carries one state here, not three (rc_config.snap_states): the
 * WebSocket loses nothing, so the older two only doubled the traffic.
 *
 * The clock is CLOCK_MONOTONIC, which the host takes from
 * performance.timeOrigin + performance.now(): one clock in every worker of
 * the page (and comparable between pages of one browser).  Random numbers come
 * from crypto.getRandomValues (WASI random_get).  The impairment knob
 * (NFSU2_ROAM_IMPAIR) works on received datagrams as on the Mac. */
#include "roam_plat.h"
#include "host_abi.h"
#include "net_proto.h"
#include "gpu_proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>

static pthread_t       g_thr;
static int             g_thr_on;
static atomic_int      g_stop;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static rc_session     *g_s;
static roam_plat_cfg   g_cfg;
static roam_plat_stats g_st;          /* written under g_mu */
static net_mailbox    *MB;            /* allocated once, registered once */
/* the browser's own numbers (under g_mu) */
static struct {
    uint64_t offline, unreachable, ring_full, filtered;
    uint32_t offline_noted;                         /* generation + 1 of the outage last noted */
    double   rxw_sum, rxw_max; uint64_t rxw_n;     /* page queued -> network thread took it, ms */
} W;

void roam_plat_lock(void){ pthread_mutex_lock(&g_mu); }
void roam_plat_unlock(void){ pthread_mutex_unlock(&g_mu); }
int64_t roam_plat_now_us(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static double now_ms(void){ return (double)roam_plat_now_us() / 1000.0; }
uint32_t roam_plat_random(void){ uint32_t r; do arc4random_buf(&r, sizeof r); while (!r); return r; }
const char *roam_ip_str(uint32_t ip, char *b){
    snprintf(b, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255u, (ip >> 8) & 255u, ip & 255u); return b; }
void roam_plat_get_stats(roam_plat_stats *o){ roam_plat_lock(); *o = g_st; roam_plat_unlock(); }

static int env_on(const char *k){ const char *v = getenv(k); return v && *v && strcmp(v, "0") != 0; }
static int webtest_vlan(void){ const char *v = getenv("NFSU2_ROAM_WEBTEST"); return v && !strcasecmp(v, "vlan"); }
int roam_plat_test_mode(void){
    if (webtest_vlan()) return 0;
    return env_on("NFSU2_ROAM_HARNESS") || env_on("NFSU2_NET_HARNESS") || env_on("NFSU2_INPUT_SCRIPT") ||
           env_on("NFSU2_QUIT_FRAMES") || env_on("NFSU2_TIMEOUT") || env_on("ALARM") || env_on("NOFOCUS");
}

/* ---- the mailbox ------------------------------------------------------------- */
static inline uint32_t ld(const uint32_t *p){ return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static inline void st(uint32_t *p, uint32_t v){ __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static inline net_slot *tx_slot(uint32_t i){ return (net_slot *)((uint8_t *)MB + MB->tx_off + (i % NET_TX_SLOTS) * NET_SLOT_BYTES); }
static inline net_slot *rx_slot(uint32_t i){ return (net_slot *)((uint8_t *)MB + MB->rx_off + (i % NET_RX_SLOTS) * NET_SLOT_BYTES); }

static int mailbox_once(void){
    if (MB) return 0;
    net_mailbox *m = aligned_alloc(64, NET_MAILBOX_BYTES);
    if (!m){ fprintf(stderr, "[roam] no memory for the net mailbox\n"); return -1; }
    memset(m, 0, NET_MAILBOX_BYTES);
    m->magic = NET_MAGIC; m->version = NET_VERSION; m->hdr_bytes = NET_HDR_BYTES; m->slot_bytes = NET_SLOT_BYTES;
    m->tx_slots = NET_TX_SLOTS; m->rx_slots = NET_RX_SLOTS;
    m->tx_off = NET_HDR_BYTES; m->rx_off = NET_HDR_BYTES + NET_TX_SLOTS * NET_SLOT_BYTES;
    MB = m;
    nfsu2_register(HOST_KIND_NET, m, NET_MAILBOX_BYTES);
    return 0;
}

/* want (net_proto.h): free roam needs the switch while it runs; the page lets it go when nothing wants it */
static void set_want(uint32_t on){
    st(&MB->want, on);
    __atomic_add_fetch(&MB->tx_bell, 1, __ATOMIC_SEQ_CST);
    __builtin_wasm_memory_atomic_notify((int *)&MB->tx_bell, 1);
}

/* queue one datagram for the page (with the lock held) */
static int tx_push(uint32_t kind, uint32_t dst, uint16_t dport, const uint8_t *buf, int len){
    uint32_t h = MB->tx_head, t = ld(&MB->tx_tail);
    if (h - t >= NET_TX_SLOTS){ W.ring_full++; return 0; }
    net_slot *s = tx_slot(h);
    s->len = (uint32_t)len; s->kind = kind; s->src_ip = ld(&MB->vip); s->dst_ip = dst;
    s->src_port = g_cfg.port; s->dst_port = dport; s->t_ms = now_ms();
    memcpy(s->data, buf, (size_t)len);
    st(&MB->tx_head, h + 1);
    __atomic_add_fetch(&MB->tx_bell, 1, __ATOMIC_SEQ_CST);
    __builtin_wasm_memory_atomic_notify((int *)&MB->tx_bell, 1);
    return 1;
}

/* ---- the guard (as roam_bsd.c) ------------------------------------------------ */
static void blocked(const char *what, uint32_t ip){
    char b[16];
    g_st.blocked++;
    if (g_st.blocked <= 20 || g_st.blocked % 1000 == 0)
        fprintf(stderr, "[roam] BLOCKED %s %s (test harness: loopback only; %llu refused so far)\n",
                what, roam_ip_str(ip, b), (unsigned long long)g_st.blocked);
}
int roam_plat_guard(int test_mode, uint32_t ip, int is_bcast){
    if (!test_mode) return 1;
    if (is_bcast){ blocked("broadcast to", ip); return 0; }
    if ((ip >> 24) != 127){ blocked("sendto", ip); return 0; }
    return 1;
}

/* ---- sending (called with the lock held, from either thread) ------------------ */
static void send_err(const char *why, uint32_t ip, uint16_t port){
    g_st.send_err++;
    if (g_st.send_err <= 5 || g_st.send_err % 1000 == 0){
        char b[16];
        fprintf(stderr, "[roam] sendto %s:%u failed: %s (%llu send errors)\n", roam_ip_str(ip, b), port, why,
                (unsigned long long)g_st.send_err);
    }
}
static void do_send(uint32_t ip, uint16_t port, const uint8_t *buf, int len, int is_bcast){
    if (!roam_plat_guard(g_cfg.test_mode, ip, is_bcast) || !MB || !ld(&MB->mode)) return;
    if (len <= 0 || len > (int)NET_MAX_PAYLOAD){ send_err("datagram too long", ip, port); return; }
    if (ld(&MB->page_state) != NET_PAGE_ONLINE){
        /* not a send error: the page has not connected yet, or lost the
         * switch and reconnects; noted once per outage */
        W.offline++;
        uint32_t gen = ld(&MB->generation);
        if (W.offline_noted != gen + 1){
            W.offline_noted = gen + 1;
            fprintf(stderr, "[roam] the virtual LAN is not connected%s: datagrams are dropped until the page's switch "
                            "connection is up\n", gen ? " (reconnecting)" : " yet");
        }
        return;
    }
    uint32_t kind = NET_KIND_UDP, dst = ip;
    if (g_cfg.test_mode){ kind = NET_KIND_UDP_LOOP; dst = NET_BROADCAST; }     /* the switch as the loopback */
    else if (!is_bcast){
        uint32_t vip = ld(&MB->vip), mask = ld(&MB->netmask);
        if ((ip & mask) != (vip & mask)){
            W.unreachable++;
            char b[16], n[16];
            g_st.send_err++;
            if (g_st.send_err <= 5 || g_st.send_err % 1000 == 0)
                fprintf(stderr, "[roam] sendto %s:%u failed: not an address on the virtual LAN (%s/%d); in the browser "
                                "roam_peers takes the virtual addresses the page shows\n", roam_ip_str(ip, b), port,
                        roam_ip_str(vip & mask, n), __builtin_popcount(mask));
            return;
        }
        if (ip == ((vip & mask) | ~mask)) dst = NET_BROADCAST;               /* the subnet's broadcast */
    }
    if (!tx_push(kind, dst, port, buf, len)){ send_err("the page's send queue is full", ip, port); return; }
    g_st.sent++; g_st.sent_bytes += (uint64_t)len;
}
static void io_send(void *u, uint32_t ip, uint16_t port, const uint8_t *buf, int len){ (void)u; do_send(ip, port, buf, len, 0); }

/* the unit tests' piece (no interfaces in a browser: discovery is the switch's
 * broadcast); kept with roam_bsd.c's rules for tests/roam_unit.c */
static int if_skip(const char *n){
    return !strncmp(n, "lo", 2) || !strncmp(n, "utun", 4) || !strncmp(n, "awdl", 4) || !strncmp(n, "llw", 3) ||
           !strncmp(n, "ipsec", 5) || !strncmp(n, "gif", 3) || !strncmp(n, "stf", 3) || !strncmp(n, "anpi", 4) ||
           !strncmp(n, "ap", 2);
}
int roam_bcast_targets(const roam_if *ifs, int n, uint32_t *out, int max){
    int k = 0;
    if (k < max) out[k++] = 0xFFFFFFFFu;
    for (int i = 0; i < n && k < max; i++){
        const roam_if *f = &ifs[i];
        if (!f->up || f->loopback || !f->can_broadcast || if_skip(f->name) || !f->mask || f->mask == 0xFFFFFFFFu) continue;
        if (!rc_addr_private(f->addr)) continue;
        uint32_t b = (f->addr & f->mask) | ~f->mask;
        int dup = 0; for (int j = 0; j < k; j++) if (out[j] == b) dup = 1;
        if (!dup) out[k++] = b;
    }
    return k;
}
static void io_broadcast(void *u, uint16_t port, const uint8_t *buf, int len){
    (void)u;
    if (g_cfg.test_mode || !g_cfg.discovery){ roam_plat_guard(1, 0xFFFFFFFFu, 1); return; }
    do_send(NET_BROADCAST, port, buf, len, 1);                 /* every other browser on the switch */
}
void roam_game_event(int ev, int peer, const char *text);    /* roam_game.c provides the log */
int roam_game_racing(void);                                  /* roam_game.c: the player races (no notices) */

/* ---- notices: what the page shows over the game ----------------------------------
 * A friend joining free roam, going into a race, back in free roam after it, and
 * leaving, as NET_KIND_NOTICE lines on the TX ring; the page never sends them
 * on.  Joining free roam is told when the friend's car gets there: from the
 * menus (starting the game, or back from the main menu), or already in free
 * roam when we first hear them.  Pausing is free roam here (a window in the
 * background pauses; the name over the car says "paused"); the menus are not
 * told, and a shop and loading are in between: a friend who goes into a shop
 * and comes back is not announced.  A place is told once it has held for
 * 1.5 s, so a friend passing through a loading screen is not announced
 * either.  Going from free roam into a race, and quitting, are told only of
 * a friend whose car was near ours (NEAR_M): one far away is not missed.
 * While the player races (a race, or an outrun with a friend) nothing is
 * shown; the places are still followed, so no pile of old notices comes
 * when it ends.  All with the lock held. */
#define NOTICE_HOLD_US 1500000
#define NEAR_M 150.f
static struct { uint32_t id; uint8_t told, cand, roam; int64_t since; } g_where[RC_TABLE];   /* roam: "joined free roam" told since the menus */
/* the players whose joining was told: once each (with the hub, a player
 * near us now and then, or passing by, comes and goes as a peer) */
#define TOLD_IDS 256
static uint32_t g_told[TOLD_IDS]; static int g_ntold;
static int told_once(uint32_t id){
    for (int i = 0; i < g_ntold && i < TOLD_IDS; i++) if (g_told[i] == id) return 0;
    g_told[g_ntold++ % TOLD_IDS] = id;
    return 1;
}
/* (name 0: what alone, no friend's name first: the page shows it as its own lines are) */
static void notice(const char *name, const char *what){
    if (roam_game_racing()) return;
    char t[192];
    int n = name ? snprintf(t, sizeof t, "%s\x1f%s", *name ? name : "A player", what) : snprintf(t, sizeof t, "%s", what);
    if (n > (int)sizeof t - 1) n = (int)sizeof t - 1;
    if (MB && n > 0) tx_push(NET_KIND_NOTICE, 0, 0, (const uint8_t *)t, n);
}
void roam_plat_notice(const char *name, const char *what){ notice(name, what); }
/* the race lobby's panel: its state to the page (NET_KIND_LOBBY), in a race too; -1: the ring is full, again later */
int roam_plat_lobby(const char *json){
    int n = (int)strlen(json);
    if (!MB) return -1;
    if (n <= 0 || n > (int)NET_MAX_PAYLOAD) return 1;
    return tx_push(NET_KIND_LOBBY, 0, 0, (const uint8_t *)json, n) ? 1 : -1;
}
/* solid cars as the page's menu switched them (net_proto.h solid) */
int roam_plat_solid(void){
    uint32_t v = MB ? ld(&MB->solid) : 0;
    return v == 1 ? 0 : v == 2 ? 1 : -1;
}
/* quick chat: the page picks (net_proto.h say: the phrase, under a count that changes with each pick), the game
 * tells it what was said (NET_KIND_SAY) and, every frame, where the names of those who spoke are (the renderer's
 * frame, gpu_web.c gw_marks) */
void gw_marks(const GwMark *m, int n);
static uint32_t g_say_seen;
int roam_plat_quickchat(void){ return 1; }
int roam_plat_say_pick(void){
    uint32_t v = MB ? ld(&MB->say) : 0;
    if (v == g_say_seen) return -1;
    g_say_seen = v;
    return (int)(v & 0xffu);
}
int roam_plat_said(const char *json){
    int n = (int)strlen(json);
    if (!MB) return -1;
    if (n <= 0 || n > (int)NET_MAX_PAYLOAD) return 1;
    return tx_push(NET_KIND_SAY, 0, 0, (const uint8_t *)json, n) ? 1 : -1;
}
/* front bumpers: the page's "Add friend" (NET_KIND_FRIEND) */
int roam_plat_friend(const char *json){
    int n = (int)strlen(json);
    if (!MB) return -1;
    if (n <= 0 || n > (int)NET_MAX_PAYLOAD) return 1;
    return tx_push(NET_KIND_FRIEND, 0, 0, (const uint8_t *)json, n) ? 1 : -1;
}
void roam_plat_say_marks(const uint32_t *ids, const float *xys, int n){
    GwMark m[GW_MARKS_MAX];
    if (n > GW_MARKS_MAX) n = GW_MARKS_MAX;
    for (int i = 0; i < n; i++){ m[i].id = ids[i]; m[i].x = xys[3 * i]; m[i].y = xys[3 * i + 1]; m[i].s = xys[3 * i + 2]; }
    gw_marks(m, n);
}
/* the crews as the page's menu changed them (net_proto.h crews): read again
 * while the page writes them (the crews and colours first, then the number) */
uint32_t roam_plat_crews(uint32_t crews[RC_CREW_MAX], uint32_t rgb[RC_CREW_MAX]){
    for (int tries = 0; MB && tries < 4; tries++){
        uint32_t g = ld(&MB->crews_gen);
        if (!g) return 0;
        for (int i = 0; i < RC_CREW_MAX; i++){ crews[i] = i < 4 ? ld(&MB->crews[i]) : 0; rgb[i] = i < 4 ? ld(&MB->crews_rgb[i]) & 0xffffffu : 0; }
        if (ld(&MB->crews_gen) == g) return g;
    }
    return 0;
}
/* the room as the page changed it (net_proto.h room_name): read again while the page writes it (the name first, then
 * the number) */
uint32_t roam_plat_room(char name[64]){
    for (int tries = 0; MB && tries < 4; tries++){
        uint32_t g = ld(&MB->room_gen);
        if (!g) return 0;
        for (int i = 0; i < 16; i++){
            uint32_t w = ld(&MB->room_name[i]);
            for (int k = 0; k < 4; k++) name[4 * i + k] = (char)(w >> 8 * k);
        }
        name[63] = 0;
        if (ld(&MB->room_gen) == g) return g;
    }
    return 0;
}
static uint8_t place(const rc_peer *p){ return p->status == RC_ST_PAUSED ? RC_ST_ROAM : p->status; }
/* we are in free roam and the friend's car was within NEAR_M of ours (on the
 * ground) when last seen: SNAPs come only from free roam, so its newest state
 * is where it was before a race or a quit */
static int nearby(const rc_peer *p){
    if ((g_s->status != RC_ST_ROAM && g_s->status != RC_ST_PAUSED) || !g_s->nlast || !p->mo.n) return 0;
    const float *a = g_s->last[0].p, *b = p->mo.s[p->mo.n - 1].p;
    float dx = a[0] - b[0], dy = a[1] - b[1];
    return dx * dx + dy * dy <= NEAR_M * NEAR_M;
}
static void where_watch(int64_t now){
    for (int i = 0; i < RC_TABLE; i++){
        const rc_peer *p = &g_s->peer[i];
        if (p->state != RC_PS_EST || p->lost){ if (p->state != RC_PS_EST) g_where[i].id = 0; continue; }
        uint8_t st = place(p);
        if (st == RC_ST_LOADING || st == RC_ST_SHOP) continue;
        if (g_where[i].id != p->id){
            g_where[i].id = p->id; g_where[i].told = g_where[i].cand = st; g_where[i].roam = st == RC_ST_ROAM; g_where[i].since = now;
            continue;
        }
        if (st != g_where[i].cand){ g_where[i].cand = st; g_where[i].since = now; }
        if (g_where[i].cand == g_where[i].told || now - g_where[i].since < NOTICE_HOLD_US) continue;
        uint8_t was = g_where[i].told;
        g_where[i].told = g_where[i].cand;
        if (st == RC_ST_EVENT){ if (was == RC_ST_ROAM && nearby(p)) notice(p->name, " is in a race"); }
        else if (st == RC_ST_MENUS) g_where[i].roam = 0;                   /* out of free roam: not told */
        else if (!g_where[i].roam){ g_where[i].roam = 1; told_once(p->id); notice(p->name, " joined free roam"); }
        else if (was == RC_ST_EVENT) notice(p->name, " is in free roam");
    }
}
static void io_event(void *u, int ev, int peer, const char *text){
    (void)u; roam_game_event(ev, peer, text);
    if (peer < 0 || peer >= RC_TABLE) return;
    const rc_peer *p = &g_s->peer[peer];
    if (ev == RC_EV_JOIN){
        /* in the menus, a race (or a shop, or loading): told when the car gets to free roam */
        uint8_t st = place(p);
        g_where[peer].id = p->id; g_where[peer].told = g_where[peer].cand = st; g_where[peer].roam = st == RC_ST_ROAM;
        g_where[peer].since = roam_plat_now_us();
        if (st == RC_ST_ROAM && told_once(p->id)) notice(p->name, " joined free roam");
    } else if (ev == RC_EV_LEAVE){
        /* roam_core.c drop_peer: "<name> left: <why>"; a restart joins again at once, another room was never ours,
         * one out of range (the hub's list) is still playing.  Quitting: the place held last (the
         * game's last frames may be another), and only from free roam near us */
        const char *why = strstr(text, " left: ");
        why = why ? why + 7 : "";
        if (!strcmp(why, "quit")){ if (g_where[peer].id == p->id && g_where[peer].told == RC_ST_ROAM && nearby(p)) notice(p->name, " left"); }
        else if (!strcmp(why, "switched off")) notice(p->name, " turned free roam off");
        else if (!strcmp(why, "timeout") || !strcmp(why, "no longer hears us")) notice(p->name, " lost the connection");
        g_where[peer].id = 0;
    }
}

/* ---- impairment (test aid, as roam_bsd.c) ------------------------------------------ */
static struct { int on, loss, delay, jitter, dup; uint32_t rng; } IMP;
#define IMPQ 512
static struct { int used; int64_t at; uint32_t ip; uint16_t port; int len; uint8_t b[RC_MAX_DGRAM]; } g_q[IMPQ];
static uint32_t imp_rand(void){ uint32_t x = IMP.rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; IMP.rng = x ? x : 1u; return IMP.rng; }
static void imp_parse(const char *s){
    memset(&IMP, 0, sizeof IMP);
    if (!s || !*s) return;
    IMP.rng = 0x9E3779B9u;
    const char *p = s;
    while (*p){
        char k[16] = {0}; int v = 0, nk = 0;
        while (*p && *p != '=' && *p != ',' && nk < 15) k[nk++] = *p++;
        if (*p == '='){ p++; v = atoi(p); while (*p && *p != ',') p++; }
        if (*p == ',') p++;
        if (!strcmp(k, "loss")) IMP.loss = v;
        else if (!strcmp(k, "delay")) IMP.delay = v;
        else if (!strcmp(k, "jitter")) IMP.jitter = v;
        else if (!strcmp(k, "dup")) IMP.dup = v;
        else if (!strcmp(k, "seed")) IMP.rng = (uint32_t)v ? (uint32_t)v : 1u;
    }
    IMP.on = 1;
    fprintf(stderr, "[roam] impairment (test aid) on received datagrams: loss %d%%, delay %d ms, jitter +-%d ms, duplicates %d%%\n",
            IMP.loss, IMP.delay, IMP.jitter, IMP.dup);
}
static void imp_queue(int64_t now, uint32_t ip, uint16_t port, const uint8_t *b, int len){
    for (int copy = 0; copy < 2; copy++){
        if (copy == 1 && (int)(imp_rand() % 100u) >= IMP.dup) break;
        if (copy == 1) g_st.impair_dup++;
        int64_t d = (int64_t)IMP.delay * 1000;
        if (IMP.jitter) d += (int64_t)((int)(imp_rand() % (uint32_t)(2 * IMP.jitter * 1000 + 1)) - IMP.jitter * 1000);
        if (d < 0) d = 0;
        int i = 0; while (i < IMPQ && g_q[i].used) i++;
        if (i == IMPQ){ g_st.impair_drop++; return; }
        g_q[i].used = 1; g_q[i].at = now + d; g_q[i].ip = ip; g_q[i].port = port; g_q[i].len = len; memcpy(g_q[i].b, b, (size_t)len);
    }
}
static void imp_release(int64_t now){
    for (;;){
        int best = -1;
        for (int i = 0; i < IMPQ; i++) if (g_q[i].used && g_q[i].at <= now && (best < 0 || g_q[i].at < g_q[best].at)) best = i;
        if (best < 0) return;
        rc_receive(g_s, g_q[best].ip, g_q[best].port, g_q[best].b, g_q[best].len, now);
        g_q[best].used = 0;
    }
}

/* ---- network thread ---------------------------------------------------------------- */
/* the page's side of the link, logged when it changes: up (with the
 * address), down once per outage, a reminder after 10 s without it */
typedef struct { int up, nagged; uint32_t vip, gen; int64_t t_down; } link_seen;
static void link_watch(link_seen *k, int64_t now){
    uint32_t ps = ld(&MB->page_state), vip = ld(&MB->vip), gen = ld(&MB->generation);
    int up = ps == NET_PAGE_ONLINE;
    char b[16];
    if (up && (!k->up || vip != k->vip || gen != k->gen)){
        fprintf(stderr, "[roam] virtual LAN up: this browser is %s/%d on the switch (connection %u, %u on the switch)%s\n",
                roam_ip_str(vip, b), __builtin_popcount(ld(&MB->netmask)), gen, ld(&MB->switch_peers),
                k->vip && vip != k->vip ? "; a new address: friends see a new player once the old one timed out" :
                k->vip ? "; the same address again" : "");
        k->vip = vip; k->gen = gen; k->nagged = 0;
    }
    if (!up && k->up){
        fprintf(stderr, "[roam] virtual LAN down (was %s): the page is reconnecting, datagrams are dropped meanwhile\n",
                roam_ip_str(k->vip, b));
        k->t_down = now; k->nagged = 0;
    }
    k->up = up;
    if (!up && !k->nagged && now - k->t_down > 10000000){
        k->nagged = 1;
        fprintf(stderr, "[roam] the virtual LAN has not been connected for 10 s (%s): friends cannot be reached until it is\n",
                ps == NET_PAGE_NONE ? "the page has not attached the network (web/src/net attachNet)" :
                k->vip ? "the page lost the server's switch and keeps reconnecting" : "the page is still connecting to the server's switch");
    }
}
static void web_stats_log(const char *when){
    double tws = MB->tx_wait_ms_sum, twm = MB->tx_wait_ms_max; uint32_t twn = MB->tx_wait_n;
    fprintf(stderr, "[roam] virtual LAN %s: sent %llu, received %llu, dropped while offline %llu, send errors %llu (not on the LAN %llu, queue full %llu), "
                    "filtered %llu; page: switch rtt %.2f ms, peers on the switch %u, RX ring full %u, oversize %u; "
                    "C->page wait mean %.3f ms max %.2f ms, page->C wait mean %.3f ms max %.2f ms\n",
            when, (unsigned long long)g_st.sent, (unsigned long long)g_st.recv, (unsigned long long)W.offline,
            (unsigned long long)g_st.send_err, (unsigned long long)W.unreachable, (unsigned long long)W.ring_full,
            (unsigned long long)W.filtered, MB->switch_rtt_ms, ld(&MB->switch_peers), ld(&MB->rx_full_drop), ld(&MB->rx_oversize),
            twn ? tws / twn : 0.0, twm, W.rxw_n ? W.rxw_sum / (double)W.rxw_n : 0.0, W.rxw_max);
}
static void *net_main(void *u){
    (void)u;
    int64_t next_tick = 0, t_start = roam_plat_now_us(), next_stats = t_start + 30000000;
    link_seen ls = { 0, 0, 0, 0, t_start };
    while (!atomic_load(&g_stop)){
        int64_t now = roam_plat_now_us();
        uint32_t bell = ld(&MB->rx_bell);
        if (ld(&MB->rx_head) == MB->rx_tail){
            int64_t wait = next_tick - now;
            if (wait > 10000) wait = 10000;
            if (wait > 0) __builtin_wasm_memory_atomic_wait32((int *)&MB->rx_bell, (int)bell, wait * 1000);
        }
        __atomic_add_fetch(&MB->c_alive, 1, __ATOMIC_RELAXED);
        now = roam_plat_now_us();
        link_watch(&ls, now);
        uint32_t mode = ld(&MB->mode), port = ld(&MB->port), vip = ld(&MB->vip);
        double tms = (double)now / 1000.0;
        uint32_t tail = MB->rx_tail;
        for (int k = 0; k < 256 && tail != ld(&MB->rx_head); k++, tail++){
            const net_slot *s = rx_slot(tail);
            uint32_t ip = s->src_ip; uint16_t sport = s->src_port; int n = (int)s->len; int take = 0;
            if (n >= 0 && n <= (int)NET_MAX_PAYLOAD && s->dst_port == port){
                if (mode == NET_MODE_VLAN) take = s->kind == NET_KIND_UDP && (s->dst_ip == vip || s->dst_ip == NET_BROADCAST);
                else if (mode == NET_MODE_LOOP){ take = s->kind == NET_KIND_UDP_LOOP; ip = 0x7F000001u; }
            }
            roam_plat_lock();
            double w = tms - s->t_ms;
            if (w >= 0 && w < 60000){ W.rxw_sum += w; W.rxw_n++; if (w > W.rxw_max) W.rxw_max = w; }
            if (!take) W.filtered++;
            else {
                g_st.recv++; g_st.recv_bytes += (uint64_t)n;
                if (n > RC_MAX_DGRAM){ g_s->rx++; g_s->rx_bad++; }
                else if (IMP.on){
                    if ((int)(imp_rand() % 100u) < IMP.loss) g_st.impair_drop++;
                    else imp_queue(now, ip, sport, s->data, n);
                } else rc_receive(g_s, ip, sport, s->data, n, now);
            }
            roam_plat_unlock();
            st(&MB->rx_tail, tail + 1);
        }
        if (IMP.on){ roam_plat_lock(); imp_release(now); roam_plat_unlock(); }
        if (now >= next_tick){ roam_plat_lock(); rc_tick(g_s, now); where_watch(now); roam_plat_unlock(); next_tick = now + 10000; }
        if (now >= next_stats){ roam_plat_lock(); web_stats_log("stats"); roam_plat_unlock(); next_stats = now + 30000000; }
    }
    return 0;
}

int roam_plat_start(const roam_plat_cfg *cfg, rc_session *s){
    g_cfg = *cfg; g_s = s;
    memset(&g_st, 0, sizeof g_st); memset(&W, 0, sizeof W);
    if (g_cfg.test_mode && g_cfg.discovery){
        fprintf(stderr, "[roam] LAN discovery DISABLED: automated run (test harness), no broadcast will be sent; "
                        "only loopback peers from NFSU2_ROAM_PEERS\n");
        g_cfg.discovery = 0;
    }
    if (mailbox_once() != 0) return -1;
    /* drop what an earlier endpoint left; the page may still be sending its TX ring */
    st(&MB->rx_tail, ld(&MB->rx_head));
    st(&MB->port, g_cfg.port);
    st(&MB->mode, g_cfg.test_mode ? NET_MODE_LOOP : NET_MODE_VLAN);
    imp_parse(g_cfg.impair);
    memset(g_q, 0, sizeof g_q); memset(g_where, 0, sizeof g_where); g_ntold = 0;
    rc_io io = { 0, io_send, io_broadcast, io_event };
    const char *hub = getenv("NFSU2_ROAM_HUB");
    int use_hub = !g_cfg.test_mode && !(hub && !strcmp(hub, "0"));
    roam_plat_lock();
    s->io = io;
    if (use_hub){ s->conf.hub_ip = NET_ROAM_HUB; s->conf.hub_port = g_cfg.port; }
    /* the WebSocket loses no datagram, so a SNAP carries only the newest
     * state: half the bytes (a receiver the switch sheds for, see nfsu2-lan's
     * roam_queue, interpolates over the gap).  The impairment knob stands for
     * a lossy link: all three there, as on a LAN */
    s->conf.snap_states = IMP.on ? RC_SNAP_STATES : 1;
    roam_plat_unlock();
    atomic_store(&g_stop, 0);
    if (pthread_create(&g_thr, 0, net_main, 0) != 0){ st(&MB->mode, NET_MODE_CLOSED); return -1; }
    g_thr_on = 1;
    set_want(1);
    if (webtest_vlan())
        fprintf(stderr, "[roam] automated run on the virtual LAN (NFSU2_ROAM_WEBTEST=vlan): virtual addresses and discovery as "
                        "for players; the switch reaches only browsers on this server\n");
    fprintf(stderr, "[roam] virtual LAN UDP port %u open%s%s\n", g_cfg.port,
            g_cfg.test_mode ? " (test harness: loopback only; the switch stands for the loopback, 127.0.0.1:<port> "
                              "reaches the test instance on that port)" :
            use_hub ? ", the players near us from the server's free-roam hub (10.77.0.253)" : "",
            g_cfg.test_mode ? "" : g_cfg.discovery ? (use_hub ? "; LAN discovery if it does not answer" :
                                                      ", LAN discovery on (broadcast to every browser on the switch)") : "");
    return 0;
}
void roam_plat_stop(uint8_t reason){
    if (!g_thr_on) return;
    roam_plat_lock(); rc_shutdown(g_s, reason); roam_plat_unlock();      /* BYE x3: the page sends what is queued */
    atomic_store(&g_stop, 1);
    __atomic_add_fetch(&MB->rx_bell, 1, __ATOMIC_SEQ_CST);
    __builtin_wasm_memory_atomic_notify((int *)&MB->rx_bell, 1);
    pthread_join(g_thr, 0); g_thr_on = 0;
    roam_plat_lock(); web_stats_log("closed"); roam_plat_unlock();
    st(&MB->mode, NET_MODE_CLOSED);
    set_want(0);
}

int roam_plat_parse_peers(const char *list, int test_mode, uint16_t defport, uint32_t *ip, uint16_t *port, int max){
    int n = 0;
    if (!list) return 0;
    const char *p = list;
    while (*p && n < max){
        /* blanks, tabs, commas and ';' separate entries (as settings.ini and
         * the Windows plugin take them) */
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == ';') p++;
        if (!*p) break;
        char tok[64]; int k = 0;
        while (*p && *p != ',' && *p != ';' && *p != ' ' && *p != '\t' && k < 63) tok[k++] = *p++;
        tok[k] = 0;
        unsigned a, b, c, d, pt = defport; char extra;
        int got = sscanf(tok, "%u.%u.%u.%u:%u%c", &a, &b, &c, &d, &pt, &extra);
        if ((got != 4 && got != 5) || a > 255 || b > 255 || c > 255 || d > 255 || pt == 0 || pt > 65535){
            fprintf(stderr, "[roam] peer \"%s\" ignored: write a.b.c.d or a.b.c.d:port (IPv4 numbers, no names)\n", tok);
            continue;
        }
        uint32_t v = a << 24 | b << 16 | c << 8 | d;
        if (test_mode && a != 127){
            fprintf(stderr, "[roam] peer %s REFUSED: test harness, only 127.x.x.x peers\n", tok); continue; }
        ip[n] = v; port[n] = (uint16_t)pt; n++;
    }
    return n;
}
