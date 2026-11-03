/* lan_nx.c -- the game's network on the console: what the page's web/src/net/index.ts does for the browser.
 *
 * The runtime (roam_web.c: free roam; net_lan.c: LAN play and Online, the Winsock HLE) keeps its datagrams in two mailboxes
 * (runtime/web/net_proto.h, HOST_KIND_NET and HOST_KIND_LAN: a TX ring C fills, an RX ring the other side fills, and a header of
 * words).  In the browser the page carries them over one WebSocket to the server's virtual LAN ("the switch": /lan).  Here this
 * thread does it with the client in homebrew/switch-net (n2n.c): it connects while a mailbox WANTS the switch, publishes the address
 * (vip, netmask, the peers) and the page state in the headers, sends what C queued, and puts what comes into the RX rings.
 *
 * Off unless an empty file /switch/nfsu2/online is on the SD card (the sockets are not started otherwise).  The server is wss://nfsu2.xyz/lan,
 * or the first line of /switch/nfsu2/server.txt.  Not done here yet (the page does them): free roam's notices, lobby, quick chat and
 * friend panels (TX kinds 16-19 are dropped), the compact and delta encodings (the hello does not ask for them: plain frames), the
 * queue of TCP frames when the LAN RX ring is full (dropped), the automatic refusal of a LAN connect with no endpoint. */
#include "../web/net_proto.h"
#include "../../homebrew/switch-net/n2n.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

void *nfsu2_host_mailbox(int kind, uint32_t *size);
int nfsu2_notify(int *p, unsigned count);
void nfsu2_pin(unsigned mask);
double nfsu2_now_ms(void);

#define HOST_KIND_NET 4
#define HOST_KIND_LAN 7
#ifndef NX_DIR
#define NX_DIR "/switch/nfsu2"
#endif
#define DIR NX_DIR
#ifndef NET_IDLE_MS
#define NET_IDLE_MS 10000.0
#endif
#ifdef N2N_TLS
extern const char nfsu2_cacert_pem[];
void n2n_tls_set_ca(const char *pem);
#endif

enum { EP_ROAM = 0, EP_LAN = 1, NEP = 2 };
static net_mailbox *g_mb[NEP];
static n2n *g_c;
static volatile int g_online_state;          /* 0 none, 1 connecting, 2 online, 3 offline */
static double g_t_try;                                   /* nfsu2_now_ms() when the last connect started (the "on the LAN after N ms" log) */
static unsigned g_stat_in, g_stat_out, g_stat_conn, g_stat_drop_in, g_stat_drop_out;
static char g_url[200] = "wss://nfsu2.xyz/lan";
static char g_name[N2N_NAME_BYTES + 1] = "Switch";
static char g_id[72];
static volatile int g_stop;
static uint32_t g_keep_vip; static char g_keep_token[64];          /* the address the server gave, kept in address.txt (it holds for months) */
static void keep_load(void){
    FILE *f = fopen(DIR "/address.txt", "r"); unsigned v = 0; char t[64] = "";
    if (f){ if (fscanf(f, "%u %63s", &v, t) == 2){ g_keep_vip = v; snprintf(g_keep_token, sizeof g_keep_token, "%s", t); } fclose(f); }
}
static void keep_save(uint32_t vip, const char *token){
    g_keep_vip = vip; snprintf(g_keep_token, sizeof g_keep_token, "%s", token ? token : "");
    FILE *f = fopen(DIR "/address.txt", "w"); if (f){ fprintf(f, "%u %s\n", vip, g_keep_token); fclose(f); }
}
static void keep_drop(void){ g_keep_vip = 0; g_keep_token[0] = 0; remove(DIR "/address.txt"); }

static uint32_t ld(const uint32_t *p){ return atomic_load_explicit((const _Atomic uint32_t *)p, memory_order_acquire); }
static void st(uint32_t *p, uint32_t v){ atomic_store_explicit((_Atomic uint32_t *)p, v, memory_order_release); }
static uint8_t *slot_at(net_mailbox *m, uint32_t off, uint32_t i, uint32_t n){ return (uint8_t *)m + off + (size_t)(i % n) * NET_SLOT_BYTES; }
static void ring(net_mailbox *m){ atomic_fetch_add((_Atomic uint32_t *)&m->rx_bell, 1u); nfsu2_notify((int *)&m->rx_bell, 1); }

static void set_page(uint32_t s){ for (int i = 0; i < NEP; i++) if (g_mb[i]) st(&g_mb[i]->page_state, s); }

static void publish_peers(void){
    if (!g_c) return;
    uint32_t vip = n2n_vip(g_c), mask = n2n_netmask(g_c);
    int n = 0; const n2n_peer *p = n2n_peers(g_c, &n);
    uint32_t bits[NET_PEER_BITS / 32]; memset(bits, 0, sizeof bits);
    for (int i = 0; i < n; i++){
        uint32_t v = p[i].vip;
        if (vip && ((v ^ vip) & mask) != 0) continue;
        uint32_t h = v & ~mask;
        if (h < NET_PEER_BITS) bits[h >> 5] |= 1u << (h & 31);
    }
    int humans = 0; for (int i = 0; i < n; i++) if (!p[i].server) humans++;
    for (int e = 0; e < NEP; e++) if (g_mb[e]){
        for (unsigned w = 0; w < NET_PEER_BITS / 32; w++) st(&g_mb[e]->peer_bits[w], bits[w]);
        st(&g_mb[e]->switch_peers, (uint32_t)humans);
    }
}
static void publish_online(void){
    for (int e = 0; e < NEP; e++) if (g_mb[e]){
        st(&g_mb[e]->vip, n2n_vip(g_c)); st(&g_mb[e]->netmask, n2n_netmask(g_c));
        atomic_fetch_add((_Atomic uint32_t *)&g_mb[e]->generation, 1u);
        st(&g_mb[e]->page_state, NET_PAGE_ONLINE);
        ring(g_mb[e]);                                  /* C's network thread sees the address now */
    }
}

/* a frame from the switch into the RX ring of the endpoint that takes it (the page's rxFrame / lanRx) */
static int put_rx(net_mailbox *m, const n2n_event *ev){
    uint32_t head = ld(&m->rx_head), tail = ld(&m->rx_tail);
    if (head - tail >= NET_RX_SLOTS) return 0;
    net_slot *s = (net_slot *)slot_at(m, m->rx_off, head, NET_RX_SLOTS);
    s->len = (uint32_t)ev->len; s->kind = ev->kind; s->src_ip = ev->src; s->dst_ip = ev->dst;
    s->src_port = ev->sport; s->dst_port = ev->dport; s->pad = 0; s->t_ms = nfsu2_now_ms();
    memcpy(s->data, ev->data, ev->len);
    st(&m->rx_head, head + 1);
    return 1;
}
static void on_frame(const n2n_event *ev){
    if (ev->len > NET_MAX_PAYLOAD) return;
    net_mailbox *m = NULL;
    if (ev->kind == NET_KIND_LAN_UDP || ev->kind == NET_KIND_LAN_TCP || ev->kind == NET_KIND_LAN_ICMP){
        if (g_mb[EP_LAN] && ld(&g_mb[EP_LAN]->mode) == NET_MODE_LAN) m = g_mb[EP_LAN];
    } else if (ev->kind == NET_KIND_UDP || ev->kind == NET_KIND_UDP_LOOP){
        net_mailbox *r = g_mb[EP_ROAM];
        uint32_t mode = r ? ld(&r->mode) : NET_MODE_CLOSED;
        if (r && mode != NET_MODE_CLOSED && ((ev->kind == NET_KIND_UDP) == (mode == NET_MODE_VLAN))) m = r;
    }
    if (!m){ return; }
    if (put_rx(m, ev)){ g_stat_in++; ring(m); }
    else { atomic_fetch_add((_Atomic uint32_t *)&m->rx_full_drop, 1u); g_stat_drop_in++; }
}
static void on_event(void *user, const n2n_event *ev){
    (void)user;
    switch (ev->type){
    case N2N_EV_WELCOME:
        g_online_state = 2; g_stat_conn++;
        fprintf(stderr, "[net] on the virtual LAN as %u.%u.%u.%u (%.0f ms after the connect started)\n", ev->vip >> 24, ev->vip >> 16 & 255, ev->vip >> 8 & 255, ev->vip & 255, nfsu2_now_ms() - g_t_try);
        if (ev->token && ev->token[0] && (ev->vip != g_keep_vip || strcmp(ev->token, g_keep_token))) keep_save(ev->vip, ev->token);
        publish_peers(); publish_online();
        break;
    case N2N_EV_PEERS: publish_peers(); break;
    case N2N_EV_PONG:
        for (int e = 0; e < NEP; e++) if (g_mb[e]) g_mb[e]->switch_rtt_ms = ev->rtt_ms;
        break;
    case N2N_EV_QUEUE: fprintf(stderr, "[net] the server is full: place %d of %d\n", ev->pos, ev->of); break;
    case N2N_EV_FRAME: on_frame(ev); break;
    case N2N_EV_REMOVED: fprintf(stderr, "[net] removed by the operator (%s): %s\n", ev->ban ? "banned" : "kicked", ev->msg ? ev->msg : ""); break;
    case N2N_EV_CLOSED: g_online_state = 3; fprintf(stderr, "[net] the connection ended (%d)\n", ev->code); break;
    }
}

/* C -> switch: the TX rings */
static void drain_tx(void){
    for (int e = 0; e < NEP; e++){
        net_mailbox *m = g_mb[e]; if (!m) continue;
        uint32_t tail = ld(&m->tx_tail), head = ld(&m->tx_head);
        while (tail != head){
            net_slot *s = (net_slot *)slot_at(m, m->tx_off, tail, NET_TX_SLOTS);
            int is_net = s->kind >= NET_KIND_UDP && s->kind <= NET_KIND_LAN_ICMP;      /* 16-19 are for the page's own panels */
            if (is_net && g_online_state == 2 && g_c && s->len <= NET_MAX_PAYLOAD){
                uint8_t f[16 + NET_MAX_PAYLOAD];
                uint32_t vip = n2n_vip(g_c);
                f[0] = (uint8_t)s->kind; f[1] = f[2] = f[3] = 0;
                f[4] = vip >> 24; f[5] = vip >> 16; f[6] = vip >> 8; f[7] = vip;
                f[8] = s->dst_ip >> 24; f[9] = s->dst_ip >> 16; f[10] = s->dst_ip >> 8; f[11] = s->dst_ip;
                f[12] = s->src_port >> 8; f[13] = s->src_port; f[14] = s->dst_port >> 8; f[15] = s->dst_port;
                memcpy(f + 16, s->data, s->len);
                if (n2n_send_frame(g_c, f, 16 + s->len) == 0) g_stat_out++; else g_stat_drop_out++;
            } else if (is_net){
                atomic_fetch_add((_Atomic uint32_t *)&m->tx_offline_drop, 1u); g_stat_drop_out++;
            }
            tail++;
            st(&m->tx_tail, tail);
        }
    }
}

static int want_any(void){ for (int e = 0; e < NEP; e++) if (g_mb[e] && ld(&g_mb[e]->want)) return 1; return 0; }

static void attach(void){
    static const int kinds[NEP] = { HOST_KIND_NET, HOST_KIND_LAN };
    for (int e = 0; e < NEP; e++){
        if (g_mb[e]) continue;
        uint32_t sz = 0; net_mailbox *m = nfsu2_host_mailbox(kinds[e], &sz);
        if (m && sz >= NET_MAILBOX_BYTES && m->magic == NET_MAGIC){
            g_mb[e] = m;
            fprintf(stderr, "[net] the game's %s mailbox is up\n", e == EP_ROAM ? "free roam" : "LAN play");
            if (g_online_state == 2){ publish_online(); publish_peers(); }      /* the peer list too: a mailbox that comes up after the welcome saw no hosts ("no such host" for 10.77.0.1) */
            else set_page(g_online_state == 1 ? NET_PAGE_CONNECTING : g_online_state == 3 ? NET_PAGE_OFFLINE : NET_PAGE_NONE);
        }
    }
}

static void load_config(void){
    FILE *f = fopen(DIR "/server.txt", "r");
    if (f){ char l[200]; if (fgets(l, sizeof l, f)){ l[strcspn(l, "\r\n")] = 0; if (l[0]) snprintf(g_url, sizeof g_url, "%s", l); } fclose(f); }
    const char *e = getenv("NFSU2_NX_ID");                  /* the console user's (nx_main.c online_identity) */
    if (e && e[0]) snprintf(g_id, sizeof g_id, "%s", e);
    e = getenv("NFSU2_ROAM_NAME");
    if (e && e[0]) snprintf(g_name, sizeof g_name, "%s", e);
    f = g_id[0] ? NULL : fopen(DIR "/id.txt", "r");
    if (f){ if (fgets(g_id, sizeof g_id, f)) g_id[strcspn(g_id, "\r\n")] = 0; fclose(f); }
    if (!g_id[0]){
        unsigned char r[16];
#ifdef __SWITCH__
        randomGet(r, sizeof r);
#else
        for (int i = 0; i < 16; i++) r[i] = (unsigned char)rand();
#endif
        for (int i = 0; i < 16; i++) snprintf(g_id + 2 * i, 3, "%02x", r[i]);
        f = fopen(DIR "/id.txt", "w"); if (f){ fputs(g_id, f); fclose(f); }
    }
}

#ifdef __SWITCH__
/* Is the console online?  Without this the first connect ran with the Wi-Fi asleep or down (WSA 10050, or a DNS lookup that never
 * returned: b78 logs on the OLED) and the thread sat in it for the whole race.  nifm says "connected" only when the link is up. */
static int nx_internet_up(void){
    static int init = -1;
    if (init < 0) init = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
    if (!init) return 1;                                           /* cannot ask: try anyway */
    NifmInternetConnectionType t; u32 strength; NifmInternetConnectionStatus st;
    if (R_FAILED(nifmGetInternetConnectionStatus(&t, &strength, &st))) return 1;
    static int last = -2; int up = st == NifmInternetConnectionStatus_Connected;
    if (last != up){ last = up; fprintf(stderr, "[net] console %s (nifm type %d, signal %u, status %d)\n", up ? "online" : "OFFLINE: waiting for the Wi-Fi", (int)t, (unsigned)strength, (int)st); }
    return up;
}
#else
static int nx_internet_up(void){ return 1; }
#endif

static void *net_main(void *unused){
    (void)unused;
    nfsu2_pin(0x1);
    load_config();
    keep_load();
#ifdef N2N_TLS
    n2n_tls_set_ca(nfsu2_cacert_pem);
#endif
    double idle_since = 0, next_try = 0, backoff = 250; int announced_lan = -1;
    while (!g_stop){
        attach();
        double now = nfsu2_now_ms();
        int want = want_any();
        /* always open: the server sends the peer list (the Online host, 10.77.0.1, among them) only to a connection whose hello says LAN, and
         * the game's Online menu connects to that host within seconds of the free-roam link: a hello that said "no LAN" until the LAN
         * mailbox appeared left it "no such host" (WSAEHOSTUNREACH) and an error screen.  Broadcasts reach C only in LAN mode (on_frame). */
        int lan_open = 1;
        if (!g_c){
            if (want && now >= next_try && !nx_internet_up()){ next_try = now + 2000; }
            else if (want && now >= next_try){
                g_c = n2n_new(g_url, g_name, g_id, on_event, NULL);
                if (!g_c){ next_try = now + 5000; continue; }
                n2n_set_lan(g_c, lan_open, 0);
                int resumed = g_keep_vip && g_keep_token[0];
                if (resumed) n2n_set_resume(g_c, g_keep_vip, g_keep_token);
                announced_lan = lan_open;
                g_online_state = 1; set_page(NET_PAGE_CONNECTING);
                fprintf(stderr, "[net] connecting to %s (console clock %lld)\n", g_url, (long long)time(NULL));
                g_t_try = nfsu2_now_ms();
                int rc = n2n_connect(g_c, 10000);
                if (rc != 0){
                    fprintf(stderr, "[net] could not connect (%d, http %d); trying again in %.1f s\n", rc, n2n_http_status(g_c), backoff / 1000.0);
                    n2n_free(g_c); g_c = NULL; g_online_state = 3; set_page(NET_PAGE_OFFLINE);
                    if (resumed){ keep_drop(); next_try = 0; fprintf(stderr, "[net] the kept address did not work: asking for a new one\n"); }
                    else { next_try = nfsu2_now_ms() + backoff; backoff = backoff * 2 > 5000 ? 5000 : backoff * 2; }
                } else backoff = 250;
            } else { struct timespec ts = { 0, 20 * 1000000 }; nanosleep(&ts, NULL); }
            continue;
        }
        /* connected: read (a few ms), then what C queued */
        int rc = n2n_poll(g_c, 4);
        drain_tx();
        if (rc < 0){
            n2n_free(g_c); g_c = NULL; g_online_state = 3; set_page(NET_PAGE_OFFLINE);
            next_try = nfsu2_now_ms() + backoff; backoff = backoff * 2 > 5000 ? 5000 : backoff * 2;
            continue;
        }
        if (g_online_state == 2 && lan_open != announced_lan){          /* LAN play opened or closed since the hello */
            char j[64]; snprintf(j, sizeof j, "{\"t\":\"lan\",\"on\":%d}", lan_open ? 1 : 0);
            n2n_send_json(g_c, j); announced_lan = lan_open;
        }
        /* nothing wants the switch for 10 s: give the place back */
        if (want){ idle_since = 0; }
        else if (!idle_since){ idle_since = now; }
        else if (now - idle_since > NET_IDLE_MS){
            fprintf(stderr, "[net] nothing needs the switch: leaving it\n");
            n2n_close(g_c); n2n_free(g_c); g_c = NULL; g_online_state = 0; set_page(NET_PAGE_IDLE); idle_since = 0;
        }
    }
    if (g_c){ drain_tx(); n2n_close(g_c); n2n_free(g_c); g_c = NULL; }
    return NULL;
}

/* the console: only with the marker file; sockets come up here and not before */
void lan_nx_start(void){
    struct stat s;
    if (stat(DIR "/online", &s) != 0){ fprintf(stderr, "[net] off (no file online)\n"); return; }
#ifdef __SWITCH__
    Result rc = socketInitializeDefault();
    if (R_FAILED(rc)){ fprintf(stderr, "[net] sockets did not start: 0x%x\n", (unsigned)rc); return; }
#endif
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 192u * 1024u);                         /* TLS handshakes want stack */
    pthread_t th;
    if (pthread_create(&th, &at, net_main, NULL) == 0) pthread_detach(th);
    else fprintf(stderr, "[net] the network thread could not start\n");
}
void lan_nx_stop(void){ g_stop = 1; }
void lan_nx_report(FILE *f){
    fprintf(f, "network: state %d, %u connections, %u frames in, %u out, %u dropped in, %u dropped out\n", g_online_state, g_stat_conn, g_stat_in, g_stat_out, g_stat_drop_in, g_stat_drop_out);
}
