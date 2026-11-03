/* n2n.h -- a client for the NFSU2 server's virtual LAN ("the switch", web/server-rs/crates/lan), for a native
 * build (the Nintendo Switch homebrew; any POSIX host for testing).
 *
 * It does what web/src/net/index.ts does on the page: one WebSocket to /lan, a JSON hello, the welcome with the
 * virtual address (vip), the peer list, ping/pong, and the binary frames between virtual addresses (net_proto.h:
 * [0] kind [1..3] 0 [4..7] source vip [8..11] destination vip [12..13] source port [14..15] destination port
 * [16..] payload, big-endian).  It does not ask for the compact/delta encodings (hello "compact", "delta", ...),
 * so the switch sends plain frames, or plain bundles (kind 32), which this unpacks.
 *
 * One thread: n2n_poll() reads, answers the control frames, pings the switch every 10 s and calls the callback.
 * n2n_send_udp() writes straight to the socket and may be called from the same thread between polls. */
#ifndef N2N_H
#define N2N_H
#include <stddef.h>
#include <stdint.h>

#define N2N_KIND_UDP        1u
#define N2N_ROAM_HUB        0x0A4D00FDu   /* 10.77.0.253: the free-roam hub */
#define N2N_ROAM_PORT       47210u        /* free roam's UDP port */
#define N2N_MAX_PAYLOAD     1472u
#define N2N_NAME_BYTES      96

typedef struct n2n n2n;

/* the byte stream under the WebSocket: a TCP socket (n2n_sock.c) or a TLS session (n2n_tls_mbedtls.c) */
typedef struct n2n_io {
    void *ctx;
    int  (*send)(void *ctx, const void *buf, size_t n);              /* all n bytes: 0, or -1 */
    int  (*recv)(void *ctx, void *buf, size_t n, int timeout_ms);    /* >0 bytes, 0 nothing in time, -1 closed or error */
    void (*close)(void *ctx);
} n2n_io;

int n2n_tcp_connect(const char *host, int port, int timeout_ms, n2n_io *out);   /* n2n_sock.c */
int n2n_tls_connect(const char *host, int port, int timeout_ms, n2n_io *out);   /* n2n_tls_mbedtls.c (-DN2N_TLS) */

typedef struct { uint32_t vip; char name[N2N_NAME_BYTES + 1]; int server; } n2n_peer;

typedef enum {
    N2N_EV_WELCOME,   /* vip, token valid */
    N2N_EV_PEERS,     /* the peer table changed: n2n_peers() */
    N2N_EV_PONG,      /* rtt_ms */
    N2N_EV_QUEUE,     /* the server is full: place pos of total; the welcome comes at our turn */
    N2N_EV_FRAME,     /* a datagram from another player: kind, src, dst, sport, dport, data, len */
    N2N_EV_REMOVED,   /* the operator kicked or banned us: ban, msg; N2N_EV_CLOSED follows */
    N2N_EV_CLOSED     /* the connection ended: code (WebSocket close code, 0: dropped without one) */
} n2n_ev_type;

typedef struct {
    n2n_ev_type type;
    uint32_t vip;                         /* WELCOME */
    const char *token;                    /* WELCOME: keep it to resume the same address */
    double rtt_ms;                        /* PONG */
    int pos, of;                          /* QUEUE */
    uint8_t kind; uint32_t src, dst; uint16_t sport, dport; const uint8_t *data; size_t len;   /* FRAME */
    int ban; const char *msg;             /* REMOVED */
    int code;                             /* CLOSED */
} n2n_event;

typedef void (*n2n_cb)(void *user, const n2n_event *ev);

/* url: ws://host[:port][/path], wss://..., http://..., https://... (path defaults to /lan).  id: this install's
 * stable id (up to 64 bytes: bans name it), or NULL.  The names are copied. */
n2n *n2n_new(const char *url, const char *name, const char *id, n2n_cb cb, void *user);
void n2n_free(n2n *c);

/* opens the WebSocket and sends the hello; the welcome arrives through n2n_poll.  After a drop, call it again: it
 * asks for the same address with the token and the place in line it was given.  0, or a negative n2n_err. */
int  n2n_connect(n2n *c, int timeout_ms);
/* reads for up to timeout_ms and handles what came; 0, or -1 once the connection has ended (CLOSED was sent) */
int  n2n_poll(n2n *c, int timeout_ms);
void n2n_close(n2n *c);

int  n2n_online(const n2n *c);                 /* the welcome has come */
uint32_t n2n_vip(const n2n *c);
const n2n_peer *n2n_peers(const n2n *c, int *count);

int  n2n_send_udp(n2n *c, uint32_t dst, uint16_t sport, uint16_t dport, const void *data, size_t len);
int  n2n_send_frame(n2n *c, const void *frame, size_t len);  /* a whole frame as net_proto.h has it */
int  n2n_ping(n2n *c);
/* before n2n_connect: the hello says LAN play is open (lan) and which UDP ports have a socket (udp_ports, bits as net_proto.h has them, 0: not said) */
void n2n_set_lan(n2n *c, int lan, uint32_t udp_ports);
/* ask for an address kept from an earlier session (the welcome's vip and token) */
void n2n_set_resume(n2n *c, uint32_t vip, const char *token);
uint32_t n2n_netmask(const n2n *c);              /* the welcome's mask */
int  n2n_send_json(n2n *c, const char *json);    /* a control message, e.g. {"t":"lan","on":1} */

enum { N2N_ERR_URL = -1, N2N_ERR_CONNECT = -2, N2N_ERR_TLS = -3, N2N_ERR_HANDSHAKE = -4, N2N_ERR_IO = -5, N2N_ERR_REFUSED = -6 };
/* the HTTP status of the last refused handshake (403, 429, 503 ...) */
int  n2n_http_status(const n2n *c);
#endif
