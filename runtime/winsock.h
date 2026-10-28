/* winsock.h -- internal interface of the Winsock / Netbios HLE.
 *
 * runtime/winsock.c  front end: every ws2_32 import with Windows semantics,
 *                    the SOCKET table, the real BSD backend ("lan", and the
 *                    "loopback" guard used by tests), identity (host name,
 *                    LAN address, adapter MAC) and netapi32!Netbios.
 * runtime/net_vlan.c the "vlan" backend: a client of tools/vlanhub.py, a
 *                    virtual /24 broadcast domain carried over AF_UNIX
 *                    sockets, so several game instances can play on one Mac
 *                    without a single packet on the real network. */
#ifndef NFSU2_WINSOCK_H
#define NFSU2_WINSOCK_H
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

/* ---- Winsock error codes (winerror.h values) ---- */
#define WSAEINTR            10004
#define WSAEBADF            10009
#define WSAEACCES           10013
#define WSAEFAULT           10014
#define WSAEINVAL           10022
#define WSAEMFILE           10024
#define WSAEWOULDBLOCK      10035
#define WSAEINPROGRESS      10036
#define WSAEALREADY         10037
#define WSAENOTSOCK         10038
#define WSAEDESTADDRREQ     10039
#define WSAEMSGSIZE         10040
#define WSAEPROTOTYPE       10041
#define WSAENOPROTOOPT      10042
#define WSAEPROTONOSUPPORT  10043
#define WSAESOCKTNOSUPPORT  10044
#define WSAEOPNOTSUPP       10045
#define WSAEPFNOSUPPORT     10046
#define WSAEAFNOSUPPORT     10047
#define WSAEADDRINUSE       10048
#define WSAEADDRNOTAVAIL    10049
#define WSAENETDOWN         10050
#define WSAENETUNREACH      10051
#define WSAENETRESET        10052
#define WSAECONNABORTED     10053
#define WSAECONNRESET       10054
#define WSAENOBUFS          10055
#define WSAEISCONN          10056
#define WSAENOTCONN         10057
#define WSAESHUTDOWN        10058
#define WSAETIMEDOUT        10060
#define WSAECONNREFUSED     10061
#define WSAEHOSTDOWN        10064
#define WSAEHOSTUNREACH     10065
#define WSASYSNOTREADY      10091
#define WSAVERNOTSUPPORTED  10092
#define WSANOTINITIALISED   10093
#define WSAHOST_NOT_FOUND   11001
#define WSATRY_AGAIN        11002
#define WSANO_RECOVERY      11003
#define WSANO_DATA          11004
/* overlapped I/O and WSA events (winerror.h's ERROR_* values) */
#define WSA_INVALID_HANDLE      6
#define WSA_NOT_ENOUGH_MEMORY   8
#define WSA_INVALID_PARAMETER   87
#define WSA_OPERATION_ABORTED   995
#define WSA_IO_INCOMPLETE       996
#define WSA_IO_PENDING          997

/* ---- network modes (NFSU2_NET) ---- */
enum { NET_OFF = 0,      /* no network: WSAStartup fails (WSASYSNOTREADY)    */
       NET_LAN,          /* real BSD sockets, the Mac's LAN identity         */
       NET_LOOP,         /* real BSD sockets confined to 127.0.0.1 (tests)   */
       NET_VLAN };       /* virtual LAN through tools/vlanhub.py             */

/* Socket states (Windows view). */
enum { ST_NEW = 0, ST_BOUND, ST_LISTEN, ST_CONNECTING, ST_CONNECTED, ST_FAILED };

/* One guest SOCKET.  Every field is only touched with the guest lock held,
 * except `closing`, which a thread waiting in poll() (lock released) reads. */
typedef struct WsSock {
    int used;
    int fd;                 /* host fd (-1: none yet, e.g. a vlan TCP socket before connect) */
    int type;               /* 1 SOCK_STREAM, 2 SOCK_DGRAM, 3 SOCK_RAW (ICMP only) */
    int proto;
    int nb;                 /* guest FIONBIO */
    int state;
    int bound;              /* explicit or implicit bind done */
    int has_peer;           /* UDP: connect() set a default destination */
    int shut_rd, shut_wr;
    int so_broadcast, so_reuse, so_keepalive, so_oobinline, so_dontroute;
    int linger_on, linger_s;
    uint32_t rcvtimeo, sndtimeo;   /* ms, 0 = none */
    int sndbuf, rcvbuf;            /* reported values (vlan) */
    int ip_ttl;
    int pending_err;        /* SO_ERROR (WSA code), cleared when read */
    int conn_result;        /* connect outcome once known: 0 ok, else WSA code */
    uint64_t conn_ready_us; /* vlan: connect completes at this time */
    uint32_t laddr, raddr;  /* guest view, network byte order */
    uint16_t lport, rport;  /* host byte order */
    int icmp_fake;          /* raw ICMP socket that never receives (no ICMP backend) */
    int aux_fd;             /* its other end, kept open (0 = none) */
    int udp_reset;          /* connected UDP: next recv reports WSAECONNRESET */
    uint32_t echo_key[8], echo_dst[8]; int echo_n;   /* loopback raw ICMP: echo id/seq -> pinged guest address */
    /* vlan */
    uint32_t vid;           /* socket id at the hub */
    uint32_t conn_id;       /* TCP connection id at the hub */
    /* blocking calls in progress (guest lock released in poll) */
    int waiters;
    _Atomic int closing;
} WsSock;

/* Identity of this instance on its LAN. */
typedef struct {
    uint32_t ip;            /* network order; 0 = none */
    uint32_t mask;          /* network order */
    uint8_t  mac[6];
    char     name[64];      /* gethostname */
    char     ifname[32];
    int      ifindex;
} NetIdent;

/* tests/net_harness.c: one made-up interface address for
 * winsock_test_ifaces (strings; mac may be NULL). */
typedef struct { const char *name, *ip, *mask, *mac; } WsTestIf;
void winsock_test_ifaces(const WsTestIf *list, int n, const char *default_if);   /* n < 0: the real ones */
int  winsock_lan_identity(NetIdent *id, char *why, size_t whysz);
const char *winsock_lan_warning(void);  /* the last identity warning, "" = none */
int  winsock_test_route(uint32_t dst, uint32_t *out);   /* route query on that identity; 0 or WSA error */
int  winsock_session_active(void);      /* a WSAStartup without its WSACleanup (a LAN session) */
int  winsock_online(void);              /* the game's Online menu is served (browser, NFSU2_ONLINE) */
void winsock_wake(void);                /* an object was set or closed: WSAWaitForMultipleEvents looks again
                                           (win32_ext.c calls it; nothing to do while no such wait runs) */

/* ---- winsock.c services for the backends ---- */
uint64_t ws_now_us(void);
void ws_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  ws_errno_to_wsa(int e);
void ws_fd_setup(int fd);          /* O_NONBLOCK, FD_CLOEXEC, SO_NOSIGPIPE */
extern int g_ws_mode;
extern NetIdent g_ws_id;

/* ---- net_vlan.c ---- */
int  vlan_hello(const char *path, NetIdent *id);       /* 0 or WSA error */
int  vlan_open(WsSock *s);
int  vlan_bind(WsSock *s, uint32_t ip, uint16_t port);
int  vlan_connect(WsSock *s, uint32_t ip, uint16_t port);
int  vlan_listen(WsSock *s, int backlog);
int  vlan_accept(WsSock *ls, WsSock *ns);
int  vlan_sendto(WsSock *s, const uint8_t *buf, int len, uint32_t ip, uint16_t port, int *sent);
int  vlan_recvfrom(WsSock *s, uint8_t *buf, int len, int peek, uint32_t *ip, uint16_t *port, int *got);
void vlan_close(WsSock *s, int abortive);
int  vlan_conn_was_reset(WsSock *s);
void vlan_note(const char *text);                       /* marker line in the hub log */
int  vlan_alive(void);
void vlan_session_end(void);                            /* the last WSACleanup: no network session now */

#endif
