/* net_lan.c -- LAN play in the browser: winsock.c's virtual-LAN backend.
 *
 * The game's LAN code (EA DirtySock in SPEED2.EXE, EA's server.dll) is lifted
 * x86; runtime/winsock.c answers its Winsock calls with Windows semantics.  On
 * the Mac its "vlan" mode is a client of tools/vlanhub.py (runtime/net_vlan.c).
 * A page has no sockets, so here the same vlan_* backend runs over the local
 * server's virtual-LAN switch (web/server-rs/crates/lan), the one free roam uses:
 *
 *   guest threads --winsock.c--> vlan_* / vfd_* (this file) --TX ring--> page --WebSocket--> switch
 *   LAN thread (this file) <--RX ring-- page <--WebSocket-- switch <-- other browsers
 *
 * The rings are the LAN mailbox (net_proto.h layout, registered as
 * HOST_KIND_LAN; free roam has its own, HOST_KIND_NET).  The page gives every
 * browser a virtual address 10.77.x.x and routes frames by destination
 * address; everything else is decided here, in each browser for its own
 * sockets, as a real host's IP stack would.  The rules are vlanhub.py's (the
 * tested reference): Windows bind semantics (a second binder of a port needs
 * SO_REUSEADDR), ephemeral ports 49152-65534, UDP unicast by address and
 * port, broadcast (255.255.255.255 or the subnet's, 10.77.7.255) to every
 * socket bound to that port on every browser *including the sender* (Windows
 * loops its own broadcasts back), a "port unreachable" notice for a unicast
 * nobody listens to, delivered to the socket that sent it (a connected UDP
 * socket's next recv then fails WSAECONNRESET; the receiving browser only
 * knows our port, so each socket remembers where it sent lately), TCP with
 * refused (RST, no listener or backlog + 4 full), host unreachable after 3 s
 * (no such address on the switch), FIN and RST (abortive close), ICMP echo
 * answered by the receiving browser, raw sockets seeing every ICMP message.
 * Traffic to the own address or 127/8 never leaves the browser (server.dll's
 * services and the host's own lobby login are local connections).
 *
 * TCP over the switch: the WebSocket is reliable and ordered, and the switch
 * relays one sender's frames in order, so a stream needs no retransmission:
 * SYN -> SYNACK | RST, then DATA, FIN, RST, each frame naming the connection
 * by its 4-tuple and the connector's random id (net_proto.h, LAN header).  A
 * connection is complete on the acceptor's side when the SYN arrives (data may
 * wait in it before accept(), as in a kernel's accept queue).  Flow control:
 * a send takes only as many TX slots as are free, less TX_RESERVE kept for
 * the control frames (else WSAEWOULDBLOCK); a control frame (SYNACK, FIN,
 * RST) that still finds the ring full waits in a queue that goes out before
 * any later frame, at the latest from the LAN thread (never dropped while
 * the link is up); the page never drops a TCP frame (it queues while the RX
 * ring is full).  When a
 * browser leaves the switch the page tells us (NET_LAN_TCP_GONE) and its
 * connections are reset; when our own switch connection drops, every remote
 * connection is reset (frames were lost meanwhile).
 *
 * Virtual fds (lan_vfd.h): winsock.c's vlan code treats a socket as an fd
 * (poll, send/recv on streams, shutdown, close).  Each virtual socket is a
 * number VFD_BASE + n; waiting is memory.atomic.wait32 on one event word that
 * every change bumps (g_seq), so a waiter (guest lock released) wakes at once.
 *
 * Threads and locks: winsock.c calls in with the guest lock held; this file's
 * state is under g_mu, which the LAN thread (a worker of the page's pool,
 * never holding the guest lock) takes too.  g_mu is never held while taking
 * the guest lock.  Addresses: winsock.c and WsSock use network order
 * (in memory a.b.c.d), the mailbox host order (a<<24 | ...). */
#include "cpu.h"
#include "winsock.h"
#include "host_abi.h"
#include "net_proto.h"
#include "lan_vfd.h"
#include "racenet.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>

void guest_wait_begin(void);
void guest_wait_end(void);
static int mailbox_once(void);

/* ---- state ------------------------------------------------------------------- */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static net_mailbox *MB;                     /* the LAN mailbox, allocated and registered once */
static uint32_t g_seq;                      /* event word: bumped on every change a waiter may want */
static int g_thread_on;
static uint32_t g_vip, g_mask;              /* host order, from the page at vlan_hello */
static int g_hello;

#define LOCAL_CAP   (1u << 20)              /* a local stream's receive buffer before the writer gets EAGAIN */
#define REMOTE_CAP  (16u << 20)             /* a remote stream's receive buffer before it is reset */
#define DGQ_MAX     1024                    /* datagrams queued per socket */
#define DGQ_BYTES   (1u << 20)
#define CONNECT_TMO 3000000ull              /* us: no answer to a SYN (vlanhub: host unreachable after 3 s) */
#define TCP_CHUNK   (NET_MAX_PAYLOAD - NET_LAN_HDR)
#define UDP_MAX     (NET_MAX_PAYLOAD - NET_LAN_HDR)

static struct {
    uint64_t tx_frames, rx_frames, tx_full, tx_offline;
    uint64_t udp_out, udp_in, udp_local, udp_noport, udp_outside, udp_qfull, udp_oversize, udp_bcast_out;
    uint64_t tcp_out, tcp_in, tcp_local, tcp_refused, tcp_unreach, tcp_timeout, tcp_rst_in, tcp_rst_out, tcp_bytes_out, tcp_bytes_in;
    uint64_t tcp_reset_link, tcp_overflow, icmp_out, icmp_in, echo_answered, bad;
    uint64_t unreach_sender, unreach_conn, unreach_first, unreach_none, ctl_waited, ctl_lost, auto_held;
} S;

static uint64_t now_us(void){ return ws_now_us(); }
static inline uint32_t ld(const uint32_t *p){ return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static inline void st(uint32_t *p, uint32_t v){ __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static inline uint32_t H(uint32_t n){ return __builtin_bswap32(n); }   /* network <-> host order (wasm is little-endian) */
static void bump(void){
    __atomic_add_fetch(&g_seq, 1, __ATOMIC_SEQ_CST);
    __builtin_wasm_memory_atomic_notify((int *)&g_seq, 0xFFFFFFFFu);
}
static int dbg(void){ static int v = -1; if (v < 0){ const char *e = getenv("NFSU2_VLAN_DEBUG"); v = e && *e && *e != '0'; } return v; }
static const char *ipn(uint32_t n, char *b){ const uint8_t *q = (const uint8_t *)&n; snprintf(b, 16, "%u.%u.%u.%u", q[0], q[1], q[2], q[3]); return b; }
static int is_loop_n(uint32_t n){ return ((const uint8_t *)&n)[0] == 127; }
static uint32_t own_n(void){ return H(g_vip); }
static int online(void){ return MB && ld(&MB->page_state) == NET_PAGE_ONLINE && ld(&MB->vip) == g_vip; }
static int in_subnet_h(uint32_t h){ return g_mask && (h & g_mask) == (g_vip & g_mask); }
static int bcast_h(uint32_t h){ return h == 0xFFFFFFFFu || (g_mask && h == ((g_vip & g_mask) | ~g_mask)); }
static int peer_present_h(uint32_t h){
    if (!MB || !in_subnet_h(h)) return 0;
    uint32_t i = h & ~g_mask;
    if (i >= NET_PEER_BITS) return 1;                    /* a network wider than the bitmap: ask the host itself */
    return (ld(&MB->peer_bits[i >> 5]) >> (i & 31u)) & 1u;
}

/* ---- virtual sockets (one per winsock SOCKET: s->vid) ------------------------- */
typedef struct Dgram { struct Dgram *next; uint32_t ip; uint16_t port, flags; int len; uint8_t data[]; } Dgram;
typedef struct VConn VConn;
/* UDP: where a socket sent lately, so a "port unreachable" notice (it names
 * only our port and the unreachable address) finds the socket that sent */
#define SENT_N 8
typedef struct { uint32_t ip; uint16_t port; uint32_t seq; } Sent;
typedef struct {
    int used, kind;                         /* 1 tcp, 2 udp, 3 icmp (raw) */
    uint32_t bip; uint16_t bport; int bound, reuse;   /* bip network order */
    Dgram *qh, *qt; int qn; uint32_t qbytes;
    int listening, backlog, pending; VConn *acc_h, *acc_t;
    int vfd;                                /* udp/icmp: always; tcp: the listener's, once listening; -1 none */
    Sent sent[SENT_N];                      /* udp: the last unicast destinations on the switch (network order) */
    int has_peer; uint32_t raddr; uint16_t rport;   /* udp: winsock.c's default destination, as of the last call */
    int rt;                                 /* the runtime's own port (vlan_rt_*): no guest socket */
} VSock;
static uint32_t g_sent_seq;
#define NVS 1024
static VSock g_vs[NVS];

enum { C_SYN_SENT = 1, C_EST, C_REFUSED, C_FAILED };
struct VConn {
    VConn *next, *prev;                     /* all connections */
    uint32_t cid;                           /* the connector's id */
    uint32_t lip, rip;                      /* network order */
    uint16_t lport, rport;
    int state, local;
    VConn *peer;                            /* local: the other end (NULL once it closed) */
    int rx_fin, rx_rst, tx_fin, rst_sent;
    uint8_t *rb; uint32_t roff, rlen, rcap;
    VSock *listener; VConn *acc_next;       /* waiting in a listener's accept queue */
    uint64_t deadline;                      /* SYN_SENT: give up then */
    int vfd;
};
static VConn *g_conns;

/* ---- virtual fds --------------------------------------------------------------- */
enum { VF_FREE = 0, VF_DGRAM, VF_LISTEN, VF_STREAM, VF_DEAD };
#define NVFD 4096
static struct { int type; void *p; } g_vf[NVFD];
static int g_vf_next;
static int vfd_new(int type, void *p){
    for (int k = 0; k < NVFD; k++){
        int i = (g_vf_next + k) % NVFD;
        if (g_vf[i].type != VF_FREE) continue;
        g_vf[i].type = type; g_vf[i].p = p; g_vf_next = (i + 1) % NVFD;
        return VFD_BASE + i;
    }
    return -1;
}
static int vfd_ix(int fd){ return fd >= VFD_BASE && fd < VFD_BASE + NVFD && g_vf[fd - VFD_BASE].type != VF_FREE ? fd - VFD_BASE : -1; }
static VConn *stream_of(int fd){ int i = vfd_ix(fd); return i >= 0 && g_vf[i].type == VF_STREAM ? (VConn *)g_vf[i].p : 0; }
static VSock *vs_of(uint32_t vid){ return vid && vid < NVS && g_vs[vid].used ? &g_vs[vid] : 0; }

/* ---- the mailbox: TX -------------------------------------------------------------- */
static inline net_slot *tx_slot(uint32_t i){ return (net_slot *)((uint8_t *)MB + MB->tx_off + (i % NET_TX_SLOTS) * NET_SLOT_BYTES); }
static inline net_slot *rx_slot(uint32_t i){ return (net_slot *)((uint8_t *)MB + MB->rx_off + (i % NET_RX_SLOTS) * NET_SLOT_BYTES); }
static uint32_t tx_free(void){ return MB ? NET_TX_SLOTS - (MB->tx_head - ld(&MB->tx_tail)) : 0; }
/* TX slots a stream's data never takes: the FIN of a closesocket right after
 * a send that filled the ring (and SYNACKs, RSTs meanwhile) still fit */
#define TX_RESERVE 4u
/* control frames that found the ring full anyway (datagrams fill it to the
 * last slot): sent in order before any later frame, by the next tx() or the
 * LAN thread's next pass; dropped only when the link goes (every remote
 * connection is reset then) */
typedef struct { uint32_t dst_h; uint16_t sport, dport; uint8_t op; uint32_t cid; } CtlFrame;
#define CTLQ_N 1024
static CtlFrame g_ctlq[CTLQ_N]; static uint32_t g_ctlq_h, g_ctlq_n;
/* one frame to the switch (g_mu held); dst_h host order, NET_BROADCAST for everyone else */
static int tx_raw(uint32_t kind, uint32_t dst_h, uint16_t sport, uint16_t dport, uint8_t op, uint8_t ttl, uint32_t cid,
                  const uint8_t *data, uint32_t n){
    if (!online()){ S.tx_offline++; return 0; }
    if (n > NET_MAX_PAYLOAD - NET_LAN_HDR){ S.bad++; return 0; }
    uint32_t h = MB->tx_head;
    if (h - ld(&MB->tx_tail) >= NET_TX_SLOTS){ S.tx_full++; return 0; }
    net_slot *s = tx_slot(h);
    s->len = NET_LAN_HDR + n; s->kind = kind; s->src_ip = g_vip; s->dst_ip = dst_h;
    s->src_port = sport; s->dst_port = dport; s->t_ms = (double)now_us() / 1000.0;
    s->data[0] = op; s->data[1] = ttl; s->data[2] = s->data[3] = 0;
    s->data[4] = (uint8_t)cid; s->data[5] = (uint8_t)(cid >> 8); s->data[6] = (uint8_t)(cid >> 16); s->data[7] = (uint8_t)(cid >> 24);
    if (n) memcpy(s->data + NET_LAN_HDR, data, n);
    st(&MB->tx_head, h + 1);
    __atomic_add_fetch(&MB->tx_bell, 1, __ATOMIC_SEQ_CST);
    __builtin_wasm_memory_atomic_notify((int *)&MB->tx_bell, 1);
    S.tx_frames++;
    return 1;
}
/* the waiting control frames, as many as fit (g_mu held) */
static void ctl_flush(void){
    while (g_ctlq_n && online() && tx_free()){
        const CtlFrame *q = &g_ctlq[g_ctlq_h];
        tx_raw(NET_KIND_LAN_TCP, q->dst_h, q->sport, q->dport, q->op, 64, q->cid, 0, 0);
        g_ctlq_h = (g_ctlq_h + 1) % CTLQ_N; g_ctlq_n--;
    }
}
/* every frame but a control frame: after the waiting ones, never before */
static int tx(uint32_t kind, uint32_t dst_h, uint16_t sport, uint16_t dport, uint8_t op, uint8_t ttl, uint32_t cid,
              const uint8_t *data, uint32_t n){
    if (g_ctlq_n){ ctl_flush(); if (g_ctlq_n && online()){ S.tx_full++; return 0; } }
    return tx_raw(kind, dst_h, sport, dport, op, ttl, cid, data, n);
}
/* a TCP control frame (SYNACK, RST, FIN): sent now, or queued while the ring is full */
static void tx_ctl(uint32_t dst_h, uint16_t sport, uint16_t dport, uint8_t op, uint32_t cid){
    ctl_flush();
    if (!online()){ S.tx_offline++; return; }
    if (!g_ctlq_n && tx_free()){ tx_raw(NET_KIND_LAN_TCP, dst_h, sport, dport, op, 64, cid, 0, 0); return; }
    if (g_ctlq_n == CTLQ_N){
        if (!S.ctl_lost++) ws_log("virtual LAN: %u TCP control frames wait for the TX ring: one dropped", CTLQ_N);
        return;
    }
    S.tx_full++; S.ctl_waited++;
    g_ctlq[(g_ctlq_h + g_ctlq_n++) % CTLQ_N] = (CtlFrame){ dst_h, sport, dport, op, cid };
}
static void tcp_ctl(VConn *k, uint8_t op){
    tx_ctl(H(k->rip), k->lport, k->rport, op, k->cid);
}

/* ---- sockets: allocation, ports ------------------------------------------------------ */
static void dg_free_all(VSock *v){
    while (v->qh){ Dgram *d = v->qh; v->qh = d->next; free(d); }
    v->qt = 0; v->qn = 0; v->qbytes = 0;
}
static int port_used(int tcp, uint16_t port, uint32_t ip, VSock *except, int *reuse_all){
    int used = 0; *reuse_all = 1;
    for (int i = 1; i < NVS; i++){
        VSock *o = &g_vs[i];
        if (!o->used || o == except || !o->bound || (o->kind == 1) != tcp || o->bport != port) continue;
        if (o->bip == ip || !o->bip || !ip){ used = 1; if (!o->reuse) *reuse_all = 0; }
    }
    return used;
}
static uint16_t ephemeral(int tcp){
    for (int k = 0; k < 20000; k++){
        uint32_t r; arc4random_buf(&r, sizeof r);
        uint16_t p = (uint16_t)(49152u + r % (65535u - 49152u));
        int ra; if (!port_used(tcp, p, 0, 0, &ra)) return p;
    }
    return 0;
}

/* ---- connections ---------------------------------------------------------------------- */
static VConn *conn_new(uint32_t lip, uint16_t lport, uint32_t rip, uint16_t rport, uint32_t cid, int state){
    VConn *k = calloc(1, sizeof *k);
    if (!k) return 0;
    k->vfd = vfd_new(VF_STREAM, k);
    if (k->vfd < 0){ free(k); return 0; }
    k->lip = lip; k->lport = lport; k->rip = rip; k->rport = rport; k->cid = cid; k->state = state;
    k->next = g_conns; if (g_conns) g_conns->prev = k; g_conns = k;
    return k;
}
static void conn_free(VConn *k){
    if (k->peer){ k->peer->peer = 0; }
    if (k->prev) k->prev->next = k->next; else g_conns = k->next;
    if (k->next) k->next->prev = k->prev;
    int i = vfd_ix(k->vfd);
    if (i >= 0 && g_vf[i].p == k){ g_vf[i].type = VF_FREE; g_vf[i].p = 0; }
    free(k->rb); free(k);
}
static VConn *conn_find(uint32_t rip, uint16_t rport, uint16_t lport, uint32_t cid){
    for (VConn *k = g_conns; k; k = k->next)
        if (!k->local && k->rip == rip && k->rport == rport && k->lport == lport && k->cid == cid) return k;
    return 0;
}
/* bytes into a connection's receive buffer; 0 = over the cap */
static int rb_put(VConn *k, const uint8_t *p, uint32_t n, uint32_t cap){
    if (!n) return 1;
    if (k->rlen + n > cap) return 0;
    if (k->roff && k->roff + k->rlen + n > k->rcap){ memmove(k->rb, k->rb + k->roff, k->rlen); k->roff = 0; }
    if (k->roff + k->rlen + n > k->rcap){
        uint32_t nc = k->rcap ? k->rcap : 4096;
        while (nc < k->roff + k->rlen + n) nc *= 2;
        uint8_t *nb = realloc(k->rb, nc); if (!nb) return 0;
        k->rb = nb; k->rcap = nc;
    }
    memcpy(k->rb + k->roff + k->rlen, p, n); k->rlen += n;
    return 1;
}
static void got_rst(VConn *k){
    k->rx_rst = 1; k->rlen = 0; k->roff = 0;           /* a reset discards what was not read */
    if (k->state == C_SYN_SENT) k->state = C_REFUSED;
}
/* our side resets the connection (abortive close, overflow, link loss) */
static void send_rst(VConn *k){
    if (k->rst_sent || k->rx_rst) return;
    k->rst_sent = 1;
    if (k->local){ if (k->peer) got_rst(k->peer); }
    else if (k->state == C_EST || k->state == C_SYN_SENT){ tcp_ctl(k, NET_LAN_TCP_RST); S.tcp_rst_out++; }
}
static void send_fin(VConn *k){
    if (k->tx_fin || k->rst_sent || k->rx_rst || k->state != C_EST) return;
    k->tx_fin = 1;
    if (k->local){ if (k->peer) k->peer->rx_fin = 1; }
    else tcp_ctl(k, NET_LAN_TCP_FIN);
}
/* every connection to another browser (or one browser) is reset: the frames
 * of a lost switch connection are gone, or that browser left */
static int reset_remote(uint32_t only_rip, const char *why){
    int n = 0;
    for (VConn *k = g_conns; k; k = k->next){
        if (k->local || k->rx_rst || (only_rip && k->rip != only_rip)) continue;
        if (k->state == C_SYN_SENT){ k->state = C_FAILED; n++; continue; }
        if (k->state != C_EST) continue;
        got_rst(k); n++;
    }
    if (n){ S.tcp_reset_link += (uint64_t)n; ws_log("virtual LAN: %d TCP connection(s) reset: %s", n, why); }
    return n;
}
static VSock *find_listener(uint32_t dst_n, uint16_t port){
    for (int i = 1; i < NVS; i++){
        VSock *o = &g_vs[i];
        if (o->used && o->kind == 1 && o->listening && o->bport == port &&
            (!o->bip || o->bip == dst_n || (is_loop_n(dst_n) && is_loop_n(o->bip)))) return o;
    }
    return 0;
}
static void acc_push(VSock *l, VConn *k){
    k->listener = l; k->acc_next = 0;
    if (l->acc_t) l->acc_t->acc_next = k; else l->acc_h = k;
    l->acc_t = k; l->pending++;
}

/* ---- datagrams --------------------------------------------------------------------- */
static void dg_push(VSock *v, uint32_t src_n, uint16_t sport, uint16_t flags, const uint8_t *p, int n){
    if (v->qn >= DGQ_MAX || v->qbytes + (uint32_t)n > DGQ_BYTES){ S.udp_qfull++; return; }
    Dgram *d = malloc(sizeof *d + (size_t)n);
    if (!d){ S.udp_qfull++; return; }
    d->next = 0; d->ip = src_n; d->port = sport; d->flags = flags; d->len = n;
    if (n) memcpy(d->data, p, (size_t)n);
    if (v->qt) v->qt->next = d; else v->qh = d;
    v->qt = d; v->qn++; v->qbytes += (uint32_t)n;
}
/* vlanhub's udp_targets: sockets on this browser that take a datagram to dst:dport */
static int udp_deliver(uint32_t src_n, uint16_t sport, uint32_t dst_n, uint16_t dport, const uint8_t *p, int n, int bc){
    VSock *first = 0, *exact = 0; int k = 0;
    for (int i = 1; i < NVS; i++){
        VSock *v = &g_vs[i];
        if (!v->used || v->kind != 2 || !v->bound || v->bport != dport) continue;
        if (!(!v->bip || v->bip == dst_n || (bc && v->bip == own_n()) || (is_loop_n(dst_n) && is_loop_n(v->bip)))) continue;
        if (bc){ dg_push(v, src_n, sport, 0, p, n); k++; continue; }
        if (!first) first = v;
        if (!exact && v->bip == dst_n) exact = v;
    }
    if (!bc && (exact || first)){ dg_push(exact ? exact : first, src_n, sport, 0, p, n); k = 1; }
    return k;
}
/* an ICMP message to every raw socket here */
static void icmp_deliver(uint32_t src_n, const uint8_t *p, int n){
    for (int i = 1; i < NVS; i++){
        VSock *v = &g_vs[i];
        if (v->used && v->kind == 3) dg_push(v, src_n, 0, 0, p, n);
    }
}
static void icmp_cksum(uint8_t *m, int n){
    m[2] = m[3] = 0;
    uint32_t sum = 0;
    for (int i = 0; i + 1 < n; i += 2) sum += (uint32_t)(m[i] << 8 | m[i + 1]);
    if (n & 1) sum += (uint32_t)m[n - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    sum = ~sum & 0xFFFF; m[2] = (uint8_t)(sum >> 8); m[3] = (uint8_t)sum;
}

/* "port unreachable" from the browser at src_n: our datagram from dport to
 * src_n:sport found no socket there.  The socket that sent it gets the
 * notice (vlanhub.py knows the sender; here the binders of dport remember
 * where they sent lately): the latest sender to that address, else a socket
 * connected to it, else the first binder that is not connected elsewhere
 * (a notice an unconnected socket gets is dropped by its next recv). */
static void unreach_deliver(uint32_t src_n, uint16_t sport, uint16_t dport){
    VSock *sender = 0, *conn = 0, *first = 0; uint32_t best = 0;
    for (int i = 1; i < NVS; i++){
        VSock *v = &g_vs[i];
        if (!v->used || v->kind != 2 || !v->bound || v->bport != dport) continue;
        for (int j = 0; j < SENT_N; j++)
            if (v->sent[j].seq && v->sent[j].ip == src_n && v->sent[j].port == sport &&
                (!sender || (int32_t)(v->sent[j].seq - best) > 0)){ sender = v; best = v->sent[j].seq; }
        if (!conn && v->has_peer && v->raddr == src_n && v->rport == sport) conn = v;
        if (!first && !v->has_peer) first = v;
    }
    VSock *to = sender ? sender : conn ? conn : first;
    if (sender) S.unreach_sender++; else if (conn) S.unreach_conn++; else if (first) S.unreach_first++; else S.unreach_none++;
    if (to) dg_push(to, src_n, sport, 1, 0, 0);
}
/* a unicast from v to ip:port on the switch (network order) */
static void sent_note(VSock *v, uint32_t ip, uint16_t port){
    int k = -1;                                          /* that address again, else a free entry, else the oldest */
    for (int j = 0; j < SENT_N && k < 0; j++) if (v->sent[j].seq && v->sent[j].ip == ip && v->sent[j].port == port) k = j;
    for (int j = 0; j < SENT_N && k < 0; j++) if (!v->sent[j].seq) k = j;
    if (k < 0){ k = 0; for (int j = 1; j < SENT_N; j++) if ((int32_t)(v->sent[j].seq - v->sent[k].seq) < 0) k = j; }
    if (!++g_sent_seq) ++g_sent_seq;                     /* 0 = a free entry */
    v->sent[k] = (Sent){ ip, port, g_sent_seq };
}

/* the race's peers: the addresses the game's race link (UDP 3658, a star
 * around the game's creator) exchanged datagrams with lately (sent or
 * received, at), and how their datagrams came (racenet.h: waits, and the
 * runs of datagrams that came at once, as a link that held them hands them
 * over; received only, rx_at: our own sends to a peer filled the waits of
 * its link) (g_mu held) */
#define RACE_PORT  3658
#define RACE_PEERS 16
static struct { uint32_t ip_h; uint64_t at, rx_at; RaceLink s; int gap_run; } g_race[RACE_PEERS];
static void race_note(uint32_t ip_h, int rx){
    int k = -1;
    for (int j = 0; j < RACE_PEERS && k < 0; j++) if (g_race[j].at && g_race[j].ip_h == ip_h) k = j;
    for (int j = 0; j < RACE_PEERS && k < 0; j++) if (!g_race[j].at) k = j;
    if (k < 0){ k = 0; for (int j = 1; j < RACE_PEERS; j++) if (g_race[j].at < g_race[k].at) k = j; }
    if (!g_race[k].at || g_race[k].ip_h != ip_h){ memset(&g_race[k], 0, sizeof g_race[k]); g_race[k].ip_h = ip_h; }
    uint64_t now = now_us();
    g_race[k].at = now;
    if (!rx) return;
    uint64_t gap = g_race[k].rx_at ? now - g_race[k].rx_at : 0;
    RaceLink *s = &g_race[k].s;
    if (g_race[k].rx_at && gap <= RACENET_ONCE_MS * 1000u){ if (g_race[k].gap_run) s->after++; }
    else {
        s->runs++;
        g_race[k].gap_run = gap >= RACENET_GAP_MS * 1000u;
        if (g_race[k].gap_run){ s->gaps++; s->after++; }
        if (gap / 1000u > s->gap_max_ms) s->gap_max_ms = (uint32_t)(gap / 1000u);
    }
    s->ip_h = ip_h; s->n++;
    g_race[k].rx_at = now;
}

/* ---- the LAN thread: received frames ------------------------------------------------- */
/* Answers this "host" gives by itself, not the game: a port unreachable, an
 * echo reply, a RST to a segment of no connection.  Each costs its frame's
 * size and AUTO_COST more; at most AUTO_SRC bytes a second go to one sender
 * and AUTO_ALL to all of them (a burst of 2 s each), so that someone on the
 * switch who sends stray frames cannot make this browser spend its share of
 * the switch on answers: its own traffic was dropped then (the security
 * review).  The game asks for none of them, so none waits: one over is not
 * sent (g_mu held). */
#define AUTO_COST  64u
#define AUTO_SRC   4096u
#define AUTO_ALL   32768u
typedef struct { uint64_t t, v; int set; } AutoBucket;       /* v: bytes x 10^6 */
static AutoBucket g_auto_src[256], g_auto_all;
static int auto_has(AutoBucket *b, uint64_t rate, uint64_t n, uint64_t now){
    uint64_t cap = 2u * rate * 1000000u;
    if (!b->set){ b->set = 1; b->v = cap; }
    else if (now > b->t){ b->v += (now - b->t) * rate; if (b->v > cap) b->v = cap; }
    b->t = now;
    return b->v >= n * 1000000u;
}
static int auto_ok(uint32_t src_h, uint32_t len){
    uint64_t now = now_us(), n = (uint64_t)len + AUTO_COST;
    AutoBucket *b = &g_auto_src[src_h & 255u];
    if (!auto_has(b, AUTO_SRC, n, now) | !auto_has(&g_auto_all, AUTO_ALL, n, now)){ S.auto_held++; return 0; }
    b->v -= n * 1000000u; g_auto_all.v -= n * 1000000u;
    return 1;
}
/* a RST to a segment of no connection (under the limit above): sent? */
static int rst_back(const net_slot *sl, uint32_t cid){
    if (!auto_ok(sl->src_ip, NET_LAN_HDR)) return 0;
    tx_ctl(sl->src_ip, sl->dst_port, sl->src_port, NET_LAN_TCP_RST, cid);
    return 1;
}
static void rx_frame(const net_slot *sl){
    uint32_t n = sl->len;
    if (n < NET_LAN_HDR || n > NET_MAX_PAYLOAD){ S.bad++; return; }
    const uint8_t *d = sl->data;
    uint8_t op = d[0];
    uint32_t cid = (uint32_t)d[4] | (uint32_t)d[5] << 8 | (uint32_t)d[6] << 16 | (uint32_t)d[7] << 24;
    const uint8_t *p = d + NET_LAN_HDR; int pn = (int)(n - NET_LAN_HDR);
    uint32_t src_n = H(sl->src_ip), dst_h = sl->dst_ip;
    uint16_t sport = sl->src_port, dport = sl->dst_port;
    int bc = dst_h == NET_BROADCAST || bcast_h(dst_h);
    if (!bc && dst_h != g_vip){ S.bad++; return; }             /* not for us (the switch routes by address) */
    S.rx_frames++;
    char b1[16];
    switch (sl->kind){
    case NET_KIND_LAN_UDP:
        if (op == NET_LAN_UDP_UNREACH){                          /* to the socket that sent from dport */
            unreach_deliver(src_n, sport, dport);
            break;
        }
        S.udp_in++;
        if (!bc && (sport == RACE_PORT || dport == RACE_PORT)) race_note(sl->src_ip, 1);
        if (!udp_deliver(src_n, sport, bc ? 0xFFFFFFFFu : own_n(), dport, p, pn, bc) && !bc){
            S.udp_noport++;
            if (auto_ok(sl->src_ip, NET_LAN_HDR))
                tx(NET_KIND_LAN_UDP, sl->src_ip, dport, sport, NET_LAN_UDP_UNREACH, 64, 0, 0, 0);   /* "ICMP port unreachable" */
        }
        break;
    case NET_KIND_LAN_ICMP:
        if (pn < 8) break;
        S.icmp_in++;
        icmp_deliver(src_n, p, pn);                              /* raw sockets see every ICMP message */
        if (p[0] == 8 && !bc && auto_ok(sl->src_ip, n)){         /* echo request: answered by this "host" */
            uint8_t rep[NET_MAX_PAYLOAD];
            memcpy(rep, p, (size_t)pn); rep[0] = 0; rep[1] = 0; icmp_cksum(rep, pn);
            if (tx(NET_KIND_LAN_ICMP, sl->src_ip, 0, 0, 0, 64, 0, rep, (uint32_t)pn)) S.echo_answered++;
        }
        break;
    case NET_KIND_LAN_TCP: {
        if (op == NET_LAN_TCP_GONE){                             /* from the page: that browser left */
            reset_remote(src_n, "the other browser left the switch");
            break;
        }
        if (bc) break;
        VConn *k = conn_find(src_n, sport, dport, cid);
        switch (op){
        case NET_LAN_TCP_SYN: {
            if (k) break;                                        /* a duplicate */
            VSock *l = find_listener(own_n(), dport);
            if (!l || l->pending >= l->backlog + 4){
                S.tcp_refused++;
                if (rst_back(sl, cid))
                    ws_log("virtual LAN: tcp %s:%u -> :%u refused (%s)", ipn(src_n, b1), sport, dport, l ? "backlog full" : "no listener");
                break;
            }
            k = conn_new(own_n(), dport, src_n, sport, cid, C_EST);
            if (!k){ rst_back(sl, cid); break; }
            acc_push(l, k);
            S.tcp_in++;
            tx_ctl(sl->src_ip, dport, sport, NET_LAN_TCP_SYNACK, cid);
            ws_log("virtual LAN: tcp %s:%u -> %s:%u accepted (queued on the listener)", ipn(src_n, b1), sport,
                   ipn(own_n(), (char[16]){0}), dport);
            break; }
        case NET_LAN_TCP_SYNACK:
            if (k && k->state == C_SYN_SENT) k->state = C_EST;
            else if (!k || k->state != C_EST) rst_back(sl, cid);
            break;
        case NET_LAN_TCP_RST:
            if (k){ S.tcp_rst_in++; if (k->state == C_SYN_SENT) S.tcp_refused++; got_rst(k); }
            break;
        case NET_LAN_TCP_DATA:
            if (!k || k->state != C_EST || k->rx_rst){
                if (!k || !k->rst_sent) rst_back(sl, cid);
                break;
            }
            if (k->rx_fin) break;
            if (!rb_put(k, p, (uint32_t)pn, REMOTE_CAP)){
                S.tcp_overflow++;
                ws_log("virtual LAN: tcp from %s:%u: %u bytes unread, the connection is reset", ipn(src_n, b1), sport, k->rlen);
                send_rst(k); got_rst(k);
                break;
            }
            S.tcp_bytes_in += (uint64_t)pn;
            break;
        case NET_LAN_TCP_FIN:
            if (k) k->rx_fin = 1;
            break;
        default: S.bad++; break;
        }
        break; }
    default: S.bad++; break;
    }
}

static void stats_line(const char *when){
    ws_log("virtual LAN %s: frames out %llu in %llu; udp out %llu (broadcast %llu, local %llu) in %llu, no port %llu, "
           "outside the LAN %llu, queue full %llu; tcp out %llu in %llu local %llu, refused %llu, unreachable %llu, "
           "timed out %llu, resets in %llu out %llu, link resets %llu, bytes out %llu in %llu; icmp out %llu in %llu, "
           "echo answered %llu, answers over the limit %llu; port unreachable to the sender %llu (connected %llu, first binder %llu, nobody %llu); "
           "TX ring full %llu (control frames waited %llu, lost %llu), offline %llu; page: switch rtt %.2f ms, RX ring full %u",
           when, (unsigned long long)S.tx_frames, (unsigned long long)S.rx_frames, (unsigned long long)S.udp_out,
           (unsigned long long)S.udp_bcast_out, (unsigned long long)S.udp_local, (unsigned long long)S.udp_in,
           (unsigned long long)S.udp_noport, (unsigned long long)S.udp_outside, (unsigned long long)S.udp_qfull,
           (unsigned long long)S.tcp_out, (unsigned long long)S.tcp_in, (unsigned long long)S.tcp_local,
           (unsigned long long)S.tcp_refused, (unsigned long long)S.tcp_unreach, (unsigned long long)S.tcp_timeout,
           (unsigned long long)S.tcp_rst_in, (unsigned long long)S.tcp_rst_out, (unsigned long long)S.tcp_reset_link,
           (unsigned long long)S.tcp_bytes_out, (unsigned long long)S.tcp_bytes_in, (unsigned long long)S.icmp_out,
           (unsigned long long)S.icmp_in, (unsigned long long)S.echo_answered, (unsigned long long)S.auto_held, (unsigned long long)S.unreach_sender,
           (unsigned long long)S.unreach_conn, (unsigned long long)S.unreach_first, (unsigned long long)S.unreach_none,
           (unsigned long long)S.tx_full, (unsigned long long)S.ctl_waited, (unsigned long long)S.ctl_lost,
           (unsigned long long)S.tx_offline, MB ? MB->switch_rtt_ms : 0.0, MB ? ld(&MB->rx_full_drop) : 0u);
}

/* One pass of the LAN thread: the page's link, then every received frame. */
static int g_was_up; static uint32_t g_gen_seen;
static int lan_pump(void){
    /* the page's switch link: frames sent while it was down are lost */
    uint32_t ps = ld(&MB->page_state), gen = ld(&MB->generation);
    int up = ps == NET_PAGE_ONLINE, closed = ps == NET_PAGE_NONE;      /* none again: the page closed the link (exit) */
    int idle = ps == NET_PAGE_IDLE;                                     /* no session wanted it: the page let it go */
    if (g_hello && ((g_was_up && !up) || (up && g_gen_seen && gen != g_gen_seen))){
        pthread_mutex_lock(&g_mu);
        g_ctlq_n = 0;                                    /* frames for the old link: its connections are reset */
        reset_remote(0, up ? "the page reconnected to the switch (frames were lost meanwhile)"
                           : closed ? "the page closed its link to the switch"
                           : idle ? "the page let the switch go (no network session)" : "the page lost the switch");
        pthread_mutex_unlock(&g_mu);
        bump();
    }
    if (g_hello && up != g_was_up)
        ws_log("virtual LAN %s", up ? "up again" : closed ? "closed: the page closed its link to the server's switch"
                                 : idle ? "idle: no network session, so the page gave its place on the server's switch back"
                                 : "down: the page is reconnecting to the server's switch");
    g_was_up = up; if (up) g_gen_seen = gen;
    if (__atomic_load_n(&g_ctlq_n, __ATOMIC_RELAXED)){ pthread_mutex_lock(&g_mu); ctl_flush(); pthread_mutex_unlock(&g_mu); }
    /* received frames */
    uint32_t tail = MB->rx_tail; int got = 0;
    for (int k = 0; k < 512 && tail != ld(&MB->rx_head); k++, tail++){
        pthread_mutex_lock(&g_mu);
        if (MB->mode == NET_MODE_LAN && g_hello) rx_frame(rx_slot(tail));
        pthread_mutex_unlock(&g_mu);
        st(&MB->rx_tail, tail + 1);
        got++;
    }
    if (got) bump();
    return got;
}
static void *lan_main(void *u){
    (void)u;
    uint64_t next_stats = now_us() + 60000000ull;
    for (;;){
        uint32_t bell = ld(&MB->rx_bell);
        /* control frames waiting for TX room: the page frees slots without a bell, so look again soon */
        if (ld(&MB->rx_head) == MB->rx_tail)
            __builtin_wasm_memory_atomic_wait32((int *)&MB->rx_bell, (int)bell,
                                                __atomic_load_n(&g_ctlq_n, __ATOMIC_RELAXED) ? 2000000ll : 50000000ll);
        __atomic_add_fetch(&MB->c_alive, 1, __ATOMIC_RELAXED);
        lan_pump();
        uint64_t t = now_us();
        if (t >= next_stats){
            next_stats = t + 60000000ull;
            if (winsock_session_active()){ pthread_mutex_lock(&g_mu); stats_line("stats"); pthread_mutex_unlock(&g_mu); }
        }
    }
    return 0;
}
/* web/test/lan_unit.c: the test plays the page and the other browsers in one
 * thread (NFSU2_LAN_NOTHREAD=1: no LAN thread; the test pumps) */
net_mailbox *vlan_test_mailbox(void){ pthread_mutex_lock(&g_mu); mailbox_once(); pthread_mutex_unlock(&g_mu); return MB; }
int vlan_test_pump(void){ return MB ? lan_pump() : 0; }

static int mailbox_once(void){
    if (MB) return 0;
    net_mailbox *m = aligned_alloc(64, NET_MAILBOX_BYTES);
    if (!m){ ws_log("virtual LAN: no memory for the LAN mailbox"); return -1; }
    memset(m, 0, NET_MAILBOX_BYTES);
    m->magic = NET_MAGIC; m->version = NET_VERSION; m->hdr_bytes = NET_HDR_BYTES; m->slot_bytes = NET_SLOT_BYTES;
    m->tx_slots = NET_TX_SLOTS; m->rx_slots = NET_RX_SLOTS;
    m->tx_off = NET_HDR_BYTES; m->rx_off = NET_HDR_BYTES + NET_TX_SLOTS * NET_SLOT_BYTES;
    m->mode = NET_MODE_LAN;
    MB = m;
    nfsu2_register(HOST_KIND_LAN, m, NET_MAILBOX_BYTES);
    return 0;
}

/* ==================================================================================== */
/* winsock.c's backend (winsock.h: vlan_*)                                               */
/* ==================================================================================== */
static void ident_from_page(NetIdent *id){
    g_vip = ld(&MB->vip); g_mask = ld(&MB->netmask);
    memset(id, 0, sizeof *id);
    id->ip = H(g_vip); id->mask = H(g_mask);
    id->mac[0] = 0x02; id->mac[1] = 0x4E; id->mac[2] = 0x56; id->mac[3] = 0x4C;   /* vlanhub's 02:4e:56:4c:c:d */
    id->mac[4] = (uint8_t)(g_vip >> 8); id->mac[5] = (uint8_t)g_vip;
    const char *nm = getenv("NFSU2_NET_NAME");
    if (nm && *nm){
        int k = 0;
        for (const char *s = nm; *s && k < 31; s++){
            char ch = *s;
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-')) ch = '-';
            id->name[k++] = ch;
        }
        id->name[k] = 0;
    } else snprintf(id->name, sizeof id->name, "VLAN-%u", g_vip & ~g_mask);   /* the host number: unique on the switch */
    { const char *em = getenv("NFSU2_NET_MAC"); unsigned v[6];
      if (em && sscanf(em, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6)
          for (int i = 0; i < 6; i++) id->mac[i] = (uint8_t)v[i]; }
    snprintf(id->ifname, sizeof id->ifname, "vlan0");
}
/* want: a network session is open (from the game's WSAStartup to its last
 * WSACleanup).  While it is 0 the page lets the switch go after a while
 * (NET_PAGE_IDLE), so a player who left the LAN and Online screens holds no
 * place on the server; it connects again when want is back. */
static void set_want(uint32_t on){
    if (!MB || ld(&MB->want) == on) return;
    st(&MB->want, on);
    __atomic_add_fetch(&MB->tx_bell, 1, __ATOMIC_SEQ_CST);
    __builtin_wasm_memory_atomic_notify((int *)&MB->tx_bell, 1);
}
/* the page's link up, at most 5 s; the guest lock is released meanwhile */
static int wait_online(void){
    uint64_t t0 = now_us();
    if (ld(&MB->page_state) == NET_PAGE_ONLINE) return 1;
    guest_wait_begin();
    while (ld(&MB->page_state) != NET_PAGE_ONLINE && now_us() - t0 < 5000000ull){
        uint32_t b = ld(&MB->rx_bell);
        if (ld(&MB->page_state) == NET_PAGE_ONLINE) break;
        __builtin_wasm_memory_atomic_wait32((int *)&MB->rx_bell, (int)b, 50000000ll);
    }
    guest_wait_end();
    return ld(&MB->page_state) == NET_PAGE_ONLINE;
}
/* The first WSAStartup: the LAN mailbox, the LAN thread, and our address from
 * the page (it connects to the switch once the mailbox is registered).  The
 * guest lock is released while it waits, at most 5 s. */
int vlan_hello(const char *path, NetIdent *id){
    (void)path;
    pthread_mutex_lock(&g_mu);
    int e = mailbox_once();
    { const char *nt = getenv("NFSU2_LAN_NOTHREAD"); if (nt && *nt == '1') g_thread_on = 1; }   /* tests */
    if (!e && !g_thread_on){
        pthread_t t;
        if (pthread_create(&t, 0, lan_main, 0) == 0){ pthread_detach(t); g_thread_on = 1; }
        else e = -1;
    }
    pthread_mutex_unlock(&g_mu);
    if (e) return WSAENETDOWN;
    uint64_t t0 = now_us();
    set_want(1);
    /* a page that never attached the network (web/src/net): waited for once,
     * then every later WSAStartup fails at once instead of stalling the game */
    static int waited_unattached;
    if (ld(&MB->page_state) == NET_PAGE_NONE && waited_unattached){ set_want(0); return WSAENETDOWN; }
    if (!wait_online()){
        uint32_t ps = ld(&MB->page_state);
        set_want(0);                                     /* no session opened (the game's WSAStartup fails) */
        if (ps == NET_PAGE_NONE) waited_unattached = 1;
        ws_log("virtual LAN: not connected after 5 s (%s): no LAN play", ps == NET_PAGE_NONE ?
               "the page has not attached the network: web/src/net attachNet" : "the page is still connecting to the server's switch");
        return WSAENETDOWN;
    }
    pthread_mutex_lock(&g_mu);
    ident_from_page(id);
    g_was_up = 1; g_gen_seen = ld(&MB->generation);          /* the link as it is now: no "up again" */
    g_hello = 1;
    pthread_mutex_unlock(&g_mu);
    char b[16];
    ws_log("virtual LAN: this browser is %s/%d on the server's switch (%.1f ms to connect)",
           ipn(id->ip, b), __builtin_popcount(g_mask), (double)(now_us() - t0) / 1000.0);
    return 0;
}
/* At every WSAStartup after the first.  A session that starts while the page
 * is not on the switch (it let it go: NET_PAGE_IDLE, or it is still
 * connecting again after that, or lost it) waits for it, at most 5 s, as
 * the first does: after an idle the switch gives another address, and a
 * session started on the old one would stay without network.  A new
 * address from the switch (it gave this browser another one after the page
 * let go, or the server restarted) is taken at the start of a session;
 * within a session the sockets are bound to the old one, so the network is
 * down for it, as with the Mac's hub. */
int vlan_alive(void){
    if (!MB || !g_hello) return 0;
    if (!winsock_session_active()){
        set_want(1);
        if (!wait_online()){
            ws_log("virtual LAN: not connected again after 5 s (%s): no LAN play",
                   ld(&MB->page_state) == NET_PAGE_IDLE ? "the page did not answer" : "the page is still connecting to the server's switch");
            set_want(0);
            return 0;
        }
    }
    if (ld(&MB->page_state) != NET_PAGE_ONLINE || ld(&MB->vip) == g_vip) return 1;
    if (winsock_session_active()) return 0;
    pthread_mutex_lock(&g_mu);
    uint32_t old = g_vip;
    ident_from_page(&g_ws_id);
    pthread_mutex_unlock(&g_mu);
    char a[16], b[16];
    ws_log("virtual LAN: the switch gave this browser a new address: %s (was %s)", ipn(H(g_vip), a), ipn(H(old), b));
    return 1;
}
/* the last WSACleanup: the network session is over (winsock.c ws_cleanup) */
void vlan_session_end(void){ set_want(0); }
void vlan_note(const char *text){ ws_log("note: %s", text); }

int vlan_open(WsSock *s){
    pthread_mutex_lock(&g_mu);
    int i = 1; while (i < NVS && g_vs[i].used) i++;
    if (i == NVS){ pthread_mutex_unlock(&g_mu); return WSAEMFILE; }
    VSock *v = &g_vs[i];
    memset(v, 0, sizeof *v);
    v->used = 1; v->kind = s->type; v->vfd = -1;
    if (s->type != 1){
        v->vfd = vfd_new(VF_DGRAM, v);
        if (v->vfd < 0){ v->used = 0; pthread_mutex_unlock(&g_mu); return WSAEMFILE; }
    }
    s->vid = (uint32_t)i; s->fd = v->vfd;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

int vlan_bind(WsSock *s, uint32_t ip, uint16_t port){
    pthread_mutex_lock(&g_mu);
    VSock *v = vs_of(s->vid);
    int e = 0; char b[16];
    if (!v) e = WSAENOTSOCK;
    else if (ip && ip != own_n() && !is_loop_n(ip)) e = WSAEADDRNOTAVAIL;
    else if (v->bound) e = WSAEINVAL;
    else {
        int tcp = v->kind == 1, reuse_all;
        if (!port){ port = ephemeral(tcp); if (!port) e = WSAENOBUFS; }
        else if (port_used(tcp, port, ip, v, &reuse_all) && !s->so_reuse){
            ws_log("virtual LAN: bind %s %s:%u -> WSAEADDRINUSE", tcp ? "tcp" : "udp", ipn(ip, b), port);
            e = WSAEADDRINUSE;
        }
        if (!e){
            v->bip = ip; v->bport = port; v->bound = 1; v->reuse = s->so_reuse;
            s->laddr = ip; s->lport = port;
            if (dbg()) ws_log("virtual LAN: bind %s %s:%u", tcp ? "tcp" : v->kind == 3 ? "icmp" : "udp", ipn(ip, b), port);
        }
    }
    pthread_mutex_unlock(&g_mu);
    return e;
}

int vlan_listen(WsSock *s, int backlog){
    pthread_mutex_lock(&g_mu);
    VSock *v = vs_of(s->vid);
    int e = 0;
    if (!v || v->kind != 1) e = WSAEINVAL;
    else {
        if (v->vfd < 0) v->vfd = vfd_new(VF_LISTEN, v);
        if (v->vfd < 0) e = WSAEMFILE;
        else {
            v->listening = 1; v->backlog = backlog > 0 ? backlog : 1;
            s->fd = v->vfd;
            char b[16];
            ws_log("virtual LAN: listening on tcp %s:%u (backlog %d)", ipn(v->bip, b), v->bport, v->backlog);
        }
    }
    pthread_mutex_unlock(&g_mu);
    return e;
}

int vlan_accept(WsSock *ls, WsSock *ns){
    pthread_mutex_lock(&g_mu);
    VSock *l = vs_of(ls->vid);
    int e = 0;
    if (!l || !l->listening) e = WSAEINVAL;
    else if (!l->acc_h) e = WSAEWOULDBLOCK;
    else {
        VConn *k = l->acc_h;
        l->acc_h = k->acc_next; if (!l->acc_h) l->acc_t = 0;
        l->pending--; k->listener = 0; k->acc_next = 0;
        ns->fd = k->vfd; ns->raddr = k->rip; ns->rport = k->rport; ns->laddr = k->lip; ns->lport = k->lport;
        ns->conn_id = k->cid; ns->vid = 0;
        if (dbg()){ char b[16]; ws_log("virtual LAN: accept tcp from %s:%u", ipn(k->rip, b), k->rport); }
    }
    pthread_mutex_unlock(&g_mu);
    return e;
}

/* Remote: the SYN goes out and the answer comes later (vlan_conn_update);
 * the browser's own address and 127/8, and an address nobody holds on the
 * switch, are answered now, as vlanhub.py answers. */
int vlan_connect(WsSock *s, uint32_t ip, uint16_t port){
    pthread_mutex_lock(&g_mu);
    VSock *v = vs_of(s->vid);
    char b[16];
    if (!v){ pthread_mutex_unlock(&g_mu); return WSAENOTSOCK; }
    if (s->fd >= 0){ VConn *old = stream_of(s->fd); if (old) conn_free(old); s->fd = -1; }   /* an earlier attempt */
    if (!v->bound){ v->bip = 0; v->bport = ephemeral(1); v->bound = 1; }
    int loop = is_loop_n(ip), self = ip == own_n();
    uint32_t lip = loop ? H(0x7F000001u) : own_n();
    s->laddr = lip; s->lport = v->bport;
    uint64_t now = now_us();
    uint32_t cid; do arc4random_buf(&cid, sizeof cid); while (!cid);
    int e = 0;
    if (loop || self){
        VSock *l = find_listener(ip, port);
        s->conn_ready_us = now;
        if (!l || l->pending >= l->backlog + 4){ S.tcp_refused++; e = WSAECONNREFUSED; }
        else {
            VConn *a = conn_new(lip, v->bport, ip, port, cid, C_EST), *z = a ? conn_new(ip, port, lip, v->bport, cid, C_EST) : 0;
            if (!z){ if (a) conn_free(a); e = WSAENOBUFS; }
            else {
                a->local = z->local = 1; a->peer = z; z->peer = a;
                acc_push(l, z);
                s->fd = a->vfd; s->conn_id = cid;
                S.tcp_local++;
                if (dbg()) ws_log("virtual LAN: tcp %s:%u -> %s:%u (in this browser)", ipn(lip, b), v->bport, ipn(ip, (char[16]){0}), port);
            }
        }
    } else if (!peer_present_h(H(ip)) || !online()){
        S.tcp_unreach++;
        ws_log("virtual LAN: tcp connect %s:%u -> no such host on the switch (WSAEHOSTUNREACH in 3 s)", ipn(ip, b), port);
        s->conn_ready_us = now + CONNECT_TMO; e = WSAEHOSTUNREACH;
    } else {
        VConn *a = conn_new(lip, v->bport, ip, port, cid, C_SYN_SENT);
        if (!a) e = WSAENOBUFS;
        else if (!tx(NET_KIND_LAN_TCP, H(ip), v->bport, port, NET_LAN_TCP_SYN, 64, cid, 0, 0)){
            conn_free(a); S.tcp_timeout++; s->conn_ready_us = now + CONNECT_TMO; e = WSAETIMEDOUT;
        } else {
            a->deadline = now + CONNECT_TMO;
            s->fd = a->vfd; s->conn_id = cid;
            s->conn_ready_us = a->deadline;                      /* until the answer: vlan_conn_update */
            S.tcp_out++;
            ws_log("virtual LAN: tcp connect %s:%u -> %s:%u", ipn(lip, b), v->bport, ipn(ip, (char[16]){0}), port);
        }
    }
    pthread_mutex_unlock(&g_mu);
    return e;
}

void vlan_conn_update(WsSock *s){
    if (s->fd < 0) return;
    pthread_mutex_lock(&g_mu);
    VConn *k = stream_of(s->fd);
    if (k){
        uint64_t now = now_us();
        if (k->state == C_SYN_SENT && now >= k->deadline){
            k->state = C_FAILED; S.tcp_timeout++;
            char b[16]; ws_log("virtual LAN: tcp connect to %s:%u: no answer in 3 s (WSAETIMEDOUT)", ipn(k->rip, b), k->rport);
        }
        if (k->state == C_EST){ s->conn_result = 0; s->conn_ready_us = 0; }
        else if (k->state == C_REFUSED){ s->conn_result = WSAECONNREFUSED; s->conn_ready_us = 0; }
        else if (k->state == C_FAILED){ s->conn_result = WSAETIMEDOUT; s->conn_ready_us = 0; }
    }
    pthread_mutex_unlock(&g_mu);
}

int vlan_sendto(WsSock *s, const uint8_t *buf, int len, uint32_t ip, uint16_t port, int *sent){
    pthread_mutex_lock(&g_mu);
    VSock *v = vs_of(s->vid);
    *sent = len;
    if (!v){ pthread_mutex_unlock(&g_mu); return WSAENOTSOCK; }
    uint32_t ip_h = H(ip);
    int loop = is_loop_n(ip), self = ip == own_n(), bc = bcast_h(ip_h);
    uint32_t src = loop ? H(0x7F000001u) : own_n();
    char b[16];
    if (v->kind == 3){                                            /* ICMP (ProtoPing) */
        S.icmp_out++;
        if (loop || self){
            if (len >= 8 && buf[0] == 8){
                uint8_t rep[2048]; int n = len < (int)sizeof rep ? len : (int)sizeof rep;
                memcpy(rep, buf, (size_t)n); rep[0] = 0; rep[1] = 0; icmp_cksum(rep, n);
                icmp_deliver(ip, rep, n);
            }
        } else if (len <= (int)UDP_MAX && in_subnet_h(ip_h) && !bc)
            tx(NET_KIND_LAN_ICMP, ip_h, 0, 0, 0, (uint8_t)s->ip_ttl, 0, buf, (uint32_t)len);
        pthread_mutex_unlock(&g_mu); bump();
        return 0;
    }
    if (len > (int)UDP_MAX){
        S.udp_oversize++;
        if (S.udp_oversize <= 5) ws_log("virtual LAN: a %d-byte datagram to %s:%u is larger than %u bytes: dropped",
                                        len, ipn(ip, b), port, UDP_MAX);
        pthread_mutex_unlock(&g_mu);
        return 0;
    }
    S.udp_out++;
    v->has_peer = s->has_peer; v->raddr = s->raddr; v->rport = s->rport;
    if (loop || self){
        S.udp_local++;
        if (!udp_deliver(src, v->bport, ip, port, buf, len, 0) && self) dg_push(v, ip, port, 1, 0, 0);   /* port unreachable */
    } else if (bc){
        S.udp_bcast_out++;
        udp_deliver(own_n(), v->bport, 0xFFFFFFFFu, port, buf, len, 1);   /* our own copy, as on Windows */
        tx(NET_KIND_LAN_UDP, NET_BROADCAST, v->bport, port, NET_LAN_UDP_DATA, (uint8_t)s->ip_ttl, 0, buf, (uint32_t)len);
    } else if (in_subnet_h(ip_h)){
        if (tx(NET_KIND_LAN_UDP, ip_h, v->bport, port, NET_LAN_UDP_DATA, (uint8_t)s->ip_ttl, 0, buf, (uint32_t)len)) sent_note(v, ip, port);
        if (port == RACE_PORT || v->bport == RACE_PORT) race_note(ip_h, 0);
    } else {
        S.udp_outside++;
        static uint32_t said[8]; static int ns;
        int k; for (k = 0; k < ns && said[k] != ip; k++) {}
        if (k == ns && ns < 8){ said[ns++] = ip; ws_log("virtual LAN: datagram to %s:%u dropped: outside the virtual LAN", ipn(ip, b), port); }
    }
    pthread_mutex_unlock(&g_mu);
    bump();
    return 0;
}

int vlan_recvfrom(WsSock *s, uint8_t *buf, int len, int peek, uint32_t *ip, uint16_t *port, int *got){
    pthread_mutex_lock(&g_mu);
    VSock *v = vs_of(s->vid);
    int e = WSAEWOULDBLOCK;
    *got = 0;
    if (v){ v->has_peer = s->has_peer; v->raddr = s->raddr; v->rport = s->rport; }
    while (v && v->qh){
        Dgram *d = v->qh;
        if (d->flags & 1){                                       /* port unreachable for this socket */
            /* a connected socket hears of its own peer only (as a kernel
             * matches an ICMP error to a connected socket's peer) */
            int ours = s->has_peer && d->ip == s->raddr && d->port == s->rport;
            v->qh = d->next; if (!v->qh) v->qt = 0; v->qn--; free(d);
            if (ours){ e = WSAECONNRESET; break; }
            continue;
        }
        *ip = d->ip; *port = d->port;
        int n = d->len;
        if (v->kind == 3){
            /* Windows raw sockets deliver the IPv4 header in front of the ICMP message */
            uint8_t ih[20]; memset(ih, 0, sizeof ih);
            int tot = n + 20;
            ih[0] = 0x45; ih[2] = (uint8_t)(tot >> 8); ih[3] = (uint8_t)tot; ih[8] = 64; ih[9] = 1;
            memcpy(ih + 12, &d->ip, 4); uint32_t me = own_n(); memcpy(ih + 16, &me, 4);
            uint32_t sum = 0; for (int i = 0; i < 20; i += 2) sum += (uint32_t)(ih[i] << 8 | ih[i + 1]);
            while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
            sum = ~sum & 0xFFFF; ih[10] = (uint8_t)(sum >> 8); ih[11] = (uint8_t)sum;
            int c1 = len < 20 ? len : 20; memcpy(buf, ih, (size_t)c1);
            int c2 = len - c1 < n ? len - c1 : n; if (c2 > 0) memcpy(buf + c1, d->data, (size_t)c2);
            if (tot > len){ *got = len; e = WSAEMSGSIZE; } else { *got = tot; e = 0; }
        } else if (n > len){ memcpy(buf, d->data, (size_t)len); *got = len; e = WSAEMSGSIZE; }   /* head copied, rest discarded */
        else { memcpy(buf, d->data, (size_t)n); *got = n; e = 0; }
        if (!peek){ v->qh = d->next; if (!v->qh) v->qt = 0; v->qn--; v->qbytes -= (uint32_t)d->len; free(d); }
        break;
    }
    pthread_mutex_unlock(&g_mu);
    return e;
}

void vlan_close(WsSock *s, int abortive){
    pthread_mutex_lock(&g_mu);
    VConn *k = s->fd >= 0 ? stream_of(s->fd) : 0;
    if (k && abortive) send_rst(k);
    VSock *v = vs_of(s->vid);
    if (v){
        if (v->listening){                                       /* connections nobody accepted: reset */
            while (v->acc_h){
                VConn *q = v->acc_h; v->acc_h = q->acc_next;
                send_rst(q); conn_free(q);
            }
            v->acc_t = 0; v->pending = 0;
        }
        dg_free_all(v);
        int i = vfd_ix(v->vfd);
        if (i >= 0 && g_vf[i].p == v){ g_vf[i].type = VF_DEAD; g_vf[i].p = 0; }
        v->used = 0;
    }
    pthread_mutex_unlock(&g_mu);
    bump();
}

int vlan_conn_was_reset(WsSock *s){
    pthread_mutex_lock(&g_mu);
    VConn *k = s->fd >= 0 ? stream_of(s->fd) : 0;
    int r = k && k->rx_rst;
    pthread_mutex_unlock(&g_mu);
    return r;
}

void vlan_fionread(WsSock *s, uint32_t *out){
    pthread_mutex_lock(&g_mu);
    *out = 0;
    VConn *k = stream_of(s->fd);
    if (k) *out = k->rlen;
    else { VSock *v = vs_of(s->vid);
           if (v && v->qh) *out = (uint32_t)v->qh->len + (v->kind == 3 ? 20u : 0u); }   /* the next datagram */
    pthread_mutex_unlock(&g_mu);
}

void vlan_report(void){
    pthread_mutex_lock(&g_mu);
    if (g_hello) stats_line("at exit");
    pthread_mutex_unlock(&g_mu);
}

/* ==================================================================================== */
/* virtual fds (lan_vfd.h)                                                              */
/* ==================================================================================== */
static short revents_of(int fd, short ev){
    int i = vfd_ix(fd);
    if (i < 0) return POLLNVAL;
    short r = 0;
    switch (g_vf[i].type){
    case VF_DGRAM: { VSock *v = g_vf[i].p; if (v->qh) r |= POLLIN; r |= POLLOUT; break; }
    case VF_LISTEN: { VSock *v = g_vf[i].p; if (v->acc_h) r |= POLLIN; break; }
    case VF_STREAM: {
        VConn *k = g_vf[i].p;
        if (k->state == C_SYN_SENT) return 0;
        if (k->state != C_EST) return POLLERR;                  /* refused / no answer */
        /* events 0: the caller waits for a connect's outcome (winsock.c's
         * blocking connect and select's wait): the connection is complete */
        if (!ev) return POLLOUT;
        if (k->rlen || k->rx_fin || k->rx_rst) r |= POLLIN;
        if (k->rx_rst) r |= POLLHUP;
        else if (k->local ? k->peer && k->peer->rlen < LOCAL_CAP : online() && tx_free() > TX_RESERVE) r |= POLLOUT;
        else if (k->local && !k->peer) r |= POLLHUP;
        break; }
    case VF_DEAD: return POLLHUP;
    default: return POLLNVAL;
    }
    return (short)(r & (ev | POLLHUP | POLLERR | POLLNVAL));
}

int vfd_poll(struct pollfd *p, nfds_t n, int ms){
    uint64_t dl = now_us() + (uint64_t)(ms > 0 ? ms : 0) * 1000ull;
    for (;;){
        uint32_t seq = ld(&g_seq);
        int ready = 0;
        pthread_mutex_lock(&g_mu);
        for (nfds_t i = 0; i < n; i++){
            p[i].revents = p[i].fd >= 0 ? revents_of(p[i].fd, p[i].events) : 0;
            if (p[i].revents) ready++;
        }
        pthread_mutex_unlock(&g_mu);
        if (ready || ms == 0) return ready;
        uint64_t now = now_us();
        if (ms > 0 && now >= dl) return 0;
        uint64_t w = ms < 0 ? 50000ull : dl - now;
        if (w > 50000ull) w = 50000ull;
        __builtin_wasm_memory_atomic_wait32((int *)&g_seq, (int)seq, (int64_t)w * 1000);
    }
}

/* winsock.c's WSAWaitForMultipleEvents: it reads the event word, looks at its
 * objects and sockets, then waits for the word to move (a datagram, a
 * connection's data, or vfd_wake when an object is set or closed), at most
 * ms: no wake-up between its look and its wait is lost (L-070) */
uint32_t vfd_seq(void){ return ld(&g_seq); }
void vfd_wait(uint32_t seen, int ms){
    if (ms > 0 && ld(&g_seq) == seen) __builtin_wasm_memory_atomic_wait32((int *)&g_seq, (int)seen, (int64_t)ms * 1000000);
}
void vfd_wake(void){ bump(); }

ssize_t vfd_send(int fd, const void *buf, size_t len, int flags){
    (void)flags;
    pthread_mutex_lock(&g_mu);
    VConn *k = stream_of(fd);
    ssize_t r = -1; int err = 0;
    if (!k) err = EBADF;
    else if (k->state != C_EST) err = ENOTCONN;
    else if (k->rx_rst || k->rst_sent || k->tx_fin) err = EPIPE;
    else if (k->local){
        VConn *pk = k->peer;
        if (!pk || pk->rx_rst) err = EPIPE;
        else {
            uint32_t room = pk->rlen < LOCAL_CAP ? LOCAL_CAP - pk->rlen : 0;
            size_t m = len < room ? len : room;
            if (!m && len) err = EAGAIN;
            else if (!rb_put(pk, buf, (uint32_t)m, LOCAL_CAP)) err = ENOBUFS;
            else r = (ssize_t)m;
        }
    } else {
        uint32_t fr = online() ? tx_free() : 0, freeslots = fr > TX_RESERVE ? fr - TX_RESERVE : 0;
        size_t off = 0;
        while (off < len && freeslots){
            size_t m = len - off < TCP_CHUNK ? len - off : TCP_CHUNK;
            if (!tx(NET_KIND_LAN_TCP, H(k->rip), k->lport, k->rport, NET_LAN_TCP_DATA, 64, k->cid, (const uint8_t *)buf + off, (uint32_t)m)) break;
            off += m; freeslots--;
        }
        if (!off && len) err = EAGAIN;
        else { r = (ssize_t)off; S.tcp_bytes_out += off; }
    }
    pthread_mutex_unlock(&g_mu);
    if (r > 0) bump();
    if (r < 0) errno = err;
    return r;
}

ssize_t vfd_recv(int fd, void *buf, size_t len, int flags){
    pthread_mutex_lock(&g_mu);
    VConn *k = stream_of(fd);
    ssize_t r = -1; int err = 0, freed = 0;
    if (!k) err = EBADF;
    else if (k->rlen){
        size_t m = len < k->rlen ? len : k->rlen;
        memcpy(buf, k->rb + k->roff, m);
        if (!(flags & MSG_PEEK)){ k->roff += (uint32_t)m; k->rlen -= (uint32_t)m; if (!k->rlen) k->roff = 0; freed = k->local; }
        r = (ssize_t)m;
    } else if (k->rx_fin || k->rx_rst || (k->local && !k->peer)) r = 0;
    else if (k->state != C_EST) err = ENOTCONN;
    else err = EAGAIN;
    pthread_mutex_unlock(&g_mu);
    if (freed) bump();                                           /* room for a local writer */
    if (r < 0) errno = err;
    return r;
}

int vfd_shutdown(int fd, int how){
    pthread_mutex_lock(&g_mu);
    VConn *k = stream_of(fd);
    if (k && (how == SHUT_WR || how == SHUT_RDWR)) send_fin(k);
    pthread_mutex_unlock(&g_mu);
    bump();                                                      /* closesocket under a waiter: wake it */
    return 0;
}

int vfd_close(int fd){
    pthread_mutex_lock(&g_mu);
    int i = vfd_ix(fd);
    if (i >= 0){
        if (g_vf[i].type == VF_STREAM){
            VConn *k = g_vf[i].p;
            if (k->listener){                                    /* never accepted (cannot happen through winsock.c) */
                VSock *l = k->listener; VConn **pp = &l->acc_h;
                while (*pp && *pp != k) pp = &(*pp)->acc_next;
                if (*pp){ *pp = k->acc_next; l->pending--; l->acc_t = 0; for (VConn *q = l->acc_h; q; q = q->acc_next) l->acc_t = q; }
            }
            if (k->state == C_SYN_SENT) send_rst(k);
            else send_fin(k);
            conn_free(k);
        } else if (g_vf[i].type == VF_DGRAM || g_vf[i].type == VF_LISTEN){
            VSock *v = g_vf[i].p;                                /* normally vlan_close came first */
            if (v){ dg_free_all(v); v->used = 0; }
            g_vf[i].type = VF_FREE; g_vf[i].p = 0;
        } else { g_vf[i].type = VF_FREE; g_vf[i].p = 0; }
    }
    pthread_mutex_unlock(&g_mu);
    bump();
    return i >= 0 ? 0 : -1;
}

/* ---- the runtime's own UDP port (racehorn.c) ------------------------------------------- */
/* Datagrams between the runtimes of the players' browsers that the game never
 * sees: a socket of ours on that port, which no guest SOCKET names (a guest
 * bind of the port finds it in use, as on a real host).  Opened by the first
 * call; 0 when the port is the game's, or before the game opened the LAN. */
static VSock *rt_sock(uint16_t port){                             /* g_mu held */
    for (int i = 1; i < NVS; i++) if (g_vs[i].used && g_vs[i].rt && g_vs[i].bport == port) return &g_vs[i];
    int reuse, i = 1;
    if (!MB || port_used(0, port, 0, 0, &reuse)) return 0;
    while (i < NVS && g_vs[i].used) i++;
    if (i == NVS) return 0;
    VSock *v = &g_vs[i];
    memset(v, 0, sizeof *v);
    v->used = 1; v->kind = 2; v->vfd = -1; v->rt = 1; v->bound = 1; v->bport = port;
    return v;
}
/* one datagram from our port to the same port at ip_h (host order) on the
 * switch: 0 sent, -1 not */
int vlan_rt_send(uint16_t port, uint32_t ip_h, const void *p, int n){
    pthread_mutex_lock(&g_mu);
    int r = -1;
    if (n >= 0 && n <= (int)UDP_MAX && in_subnet_h(ip_h) && !bcast_h(ip_h) && ip_h != g_vip && rt_sock(port) &&
        tx(NET_KIND_LAN_UDP, ip_h, port, port, NET_LAN_UDP_DATA, 64, 0, p, (uint32_t)n)){ S.udp_out++; r = 0; }
    pthread_mutex_unlock(&g_mu);
    return r;
}
/* the next datagram that came to our port: its length (cut to cap) and
 * sender (host order), -1 none */
int vlan_rt_recv(uint16_t port, uint32_t *ip_h, void *p, int cap){
    pthread_mutex_lock(&g_mu);
    VSock *v = rt_sock(port);
    int r = -1;
    while (v && v->qh && r < 0){
        Dgram *d = v->qh;
        v->qh = d->next; if (!v->qh) v->qt = 0; v->qn--; v->qbytes -= (uint32_t)d->len;
        if (!(d->flags & 1)){                                    /* not a "port unreachable" */
            r = d->len < cap ? d->len : cap;
            memcpy(p, d->data, (size_t)r); *ip_h = H(d->ip);
        }
        free(d);
    }
    pthread_mutex_unlock(&g_mu);
    return r;
}
/* the race's peers heard from or sent to within the last within_ms (host order) */
int vlan_race_peers(uint32_t *ip_h, int max, uint32_t within_ms){
    pthread_mutex_lock(&g_mu);
    uint64_t now = now_us();
    int n = 0;
    for (int j = 0; j < RACE_PEERS && n < max; j++)
        if (g_race[j].at && now - g_race[j].at <= (uint64_t)within_ms * 1000u && g_race[j].ip_h != g_vip) ip_h[n++] = g_race[j].ip_h;
    pthread_mutex_unlock(&g_mu);
    return n;
}
/* how the race peers' datagrams came since the last reset (racenet.h) */
int vlan_race_links(RaceLink *o, int max, int reset){
    pthread_mutex_lock(&g_mu);
    int n = 0;
    for (int j = 0; j < RACE_PEERS; j++){
        if (!g_race[j].at || g_race[j].ip_h == g_vip) continue;
        if (g_race[j].s.n && n < max) o[n++] = g_race[j].s;
        if (reset){ memset(&g_race[j].s, 0, sizeof g_race[j].s); g_race[j].gap_run = 0; }
    }
    pthread_mutex_unlock(&g_mu);
    return n;
}
