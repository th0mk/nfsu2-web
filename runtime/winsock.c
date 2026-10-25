/* winsock.c -- ws2_32 and netapi32!Netbios with Windows semantics.
 *
 * SPEED2.EXE's network code (EA DirtySock: LAN discovery, the lobby client,
 * the race links) and server.dll (EA's LAN server) are lifted x86, so the
 * bytes they put on the wire are the original ones.  What decides whether a
 * Mac interoperates with a Windows PC is this layer: it must answer every
 * call exactly as Winsock 2 on Windows would (structure layouts, error codes,
 * the select/connect/close semantics DirtySock relies on) and report the
 * right local identity (address, host name, adapter MAC).
 *
 * Modes (NFSU2_NET, read once):
 *   lan       real BSD sockets on the Mac's LAN (the default outside tests)
 *   off       no network: WSAStartup fails with WSASYSNOTREADY, so the game
 *             shows its own "network" error dialogs (default under the test
 *             harness, tools/autorun.sh)
 *   loopback  real BSD sockets confined to 127.0.0.1.  The guest sees a
 *             pseudo-LAN address (NFSU2_NET_ADDR, default 10.77.0.2) and one
 *             pseudo peer (NFSU2_NET_LOOPPEER, default 10.77.0.3) whose ports
 *             are mapped to loopback ports (NFSU2_NET_LOOPMAP, default
 *             "9999=19999,9900=19900,3658=13658").  Everything else is BLOCKED.
 *             NFSU2_NET_LOOPOFFSET=<n> moves the game's own ports by n on the
 *             host, so concurrent loopback runs on one Mac do not share 9999.
 *             NFSU2_NET_LOOPPEEROFFSET=<m>: the peer is a second game
 *             instance on this Mac, run with NFSU2_NET_LOOPOFFSET=<m> (and
 *             the two addresses swapped): every peer port p is host port
 *             p+m, and a datagram or connection from a host port that is not
 *             one of this instance's own sockets comes from the peer (tests
 *             of the real socket code with two instances, tests/net/loop_run.sh).
 *   vlan[:p]  virtual LAN through tools/vlanhub.py (hub socket p or
 *             NFSU2_VLAN): own 10.77.0.x address, MAC and host name per
 *             instance, broadcast, TCP, impairments, pcap.  runtime/net_vlan.c
 * In the browser (wasm) there are no BSD sockets: the only modes are off and
 * vlan, the virtual LAN of the local server's switch (web/server-rs/crates/lan),
 * whose backend is runtime/web/net_lan.c (the same vlan_* calls, and virtual
 * fds instead of AF_UNIX ones: runtime/web/lan_vfd.h).  NFSU2_NET unset, lan,
 * on or loopback all mean that virtual LAN there: a page can reach nothing but
 * the browsers on its own server.
 * Online (the game's Online menu, browser only): the server runs its own
 * stand-in for EA's long-gone servers at 10.77.0.1 on the virtual LAN
 * (web/server-rs/crates/online).  NFSU2_ONLINE (default on in the browser, off on the
 * Mac, whose hub has no such host; 0/off turns it off): in vlan mode the game's
 * lobby host string (game.h; UG2: "*pcnfs05.ea.com" at 0x7FBDBC) loses its
 * '*' in guest memory (the '*' asks DirtySock for SSL 2.0 on port 20921,
 * which the server does not speak: without it the lobby link is plain TCP to
 * 20920), and gethostbyname of that name answers 10.77.0.1.  Every other name stays
 * unresolved.  Off: the name is not found and the game shows its own "cannot
 * connect" message.
 * In a test or debug run "lan" is refused and becomes "loopback": that is
 * NFSU2_NET_HARNESS=1 (tools/autorun.sh), an input script, NFSU2_TIMEOUT, or
 * ALARM missing or non-zero.  Only the app and ./run.sh's window mode (both
 * ALARM=0) reach the real LAN; NFSU2_NET_ALLOW_LAN=1 also allows it for the
 * user's own scripted real-LAN runs (never under tools/autorun.sh).  In loopback and vlan modes no host socket can ever be created for,
 * bound to or sent to a non-loopback address: every such attempt fails with
 * WSAENETUNREACH / WSAEADDRNOTAVAIL / WSAHOST_NOT_FOUND and logs a
 * "[net] BLOCKED" line (tests/run_suite.sh fails on those).
 *
 * Identity (lan): the IPv4 address of the interface that holds the default
 * route, unless that is a tunnel (utun, ipsec, ppp...) or otherwise not a LAN
 * (awdl, llw, bridge): then the first broadcast-capable en* interface; a
 * self-assigned 169.254 address only when there is no other.
 * NFSU2_NET_ADDR=<a.b.c.d> picks the interface that holds that address,
 * NFSU2_NET_IFACE=<ifname> an interface (either may be self-assigned); a
 * choice that cannot be honoured is logged as a WARNING.  It is taken anew
 * whenever a network session starts (WSAStartup after the last WSACleanup:
 * the game's LAN screens).  The adapter MAC (Netbios NCBASTAT, the lobby's MID)
 * is that interface's hardware address when it is a universally administered
 * one, else a locally administered address derived from the host UUID (the
 * Wi-Fi "private address" changes per network); NFSU2_NET_MAC overrides it.
 * The host name is the Mac's name without ".local" (never resolved: that
 * would be mDNS traffic).
 *
 * Threads: every call runs with the guest lock held.  A call that has to wait
 * (a blocking recv/accept/connect/send, select with a timeout, a DNS lookup)
 * waits in poll() slices of at most 50 ms with the lock released
 * (guest_wait_begin/end), so other guest threads and the renderer keep
 * running; closesocket() from another thread wakes it (WSAEINTR) and the
 * host fd is closed by the last waiter, so the number cannot be reused under
 * it.  Host fds are always O_NONBLOCK, FD_CLOEXEC and SO_NOSIGPIPE.
 *
 * WSAGetLastError / WSASetLastError use the thread's TEB+0x34, the same slot
 * as GetLastError on Windows (runtime/win32.c). */
#include "cpu.h"
#include "game.h"
#include "memmap.h"
#include "winsock.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#ifndef __wasm__
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <netdb.h>
#endif
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifdef __wasm__
/* the browser's virtual LAN (runtime/web/net_lan.c): every virtual socket is a
 * virtual fd, and the fd calls of the vlan code paths below go to it */
#include "web/lan_vfd.h"
#define poll(p, n, t)        vfd_poll((p), (n), (t))
#define send(f, b, n, fl)    vfd_send((f), (b), (n), (fl))
#define recv(f, b, n, fl)    vfd_recv((f), (b), (n), (fl))
#define shutdown(f, h)       vfd_shutdown((f), (h))
#define close(f)             vfd_close(f)
/* host-fd options: only ever on a host fd, which never exists here */
#define setsockopt(f, l, o, v, n) ((void)(f), (void)(v), -1)
#define getsockopt(f, l, o, v, n) ((void)(f), (void)(v), -1)
#ifndef IFNAMSIZ
#define IFNAMSIZ 16
#endif
#endif

void guest_wait_begin(void);
void guest_wait_end(void);

int g_ws_mode = -1;
NetIdent g_ws_id;

/* ---- statistics (exit report) ---- */
static uint64_t st_sockets, st_sent, st_recv, st_bytes_out, st_bytes_in, st_blocked, st_select, st_waits;

/* ======================================================================= */
/* small helpers                                                            */
/* ======================================================================= */
static uint32_t A(CPU *c, int i){ return MR32(c->esp + 4u*(uint32_t)i); }
uint64_t ws_now_us(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}
void ws_log(const char *fmt, ...){
    char b[600]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fprintf(stderr, "[net] %s\n", b);
}
/* guest range [p, p+n) committed (n may be 0) */
static int gok(uint32_t p, uint32_t n){
    if (!p) return 0;
    if (!n) return mem_is_committed(p);
    if ((uint64_t)p + n > 0x100000000ull) return 0;
    for (uint64_t a = p & ~0x3FFFull; a < (uint64_t)p + n; a += 0x4000ull)
        if (!mem_is_committed((uint32_t)a)) return 0;
    return 1;
}
static const char *ipstr(uint32_t ip, char *b){      /* ip: network order */
    const uint8_t *q = (const uint8_t *)&ip;
    snprintf(b, 16, "%u.%u.%u.%u", q[0], q[1], q[2], q[3]); return b;
}
static uint32_t mkip(int a, int b, int c, int d){
    uint32_t v; uint8_t *q = (uint8_t *)&v; q[0] = (uint8_t)a; q[1] = (uint8_t)b; q[2] = (uint8_t)c; q[3] = (uint8_t)d;
    return v;
}
static int is_loop(uint32_t ip){ return ((const uint8_t *)&ip)[0] == 127; }
static int parse_ip(const char *s, uint32_t *out){
    struct in_addr a; if (!s || inet_pton(AF_INET, s, &a) != 1) return 0;
    *out = a.s_addr; return 1;
}

/* ---- per-thread last error: TEB+0x34, as GetLastError on Windows ----
 * One implementation for the whole HLE: win32.c's win32_set_last_error /
 * win32_get_last_error (tests/net_harness.c has its own stand-ins). */
void win32_set_last_error(CPU *c, uint32_t e);
uint32_t win32_get_last_error(CPU *c);
static _Thread_local CPU *g_curc;                    /* the calling guest thread (one host thread each) */
static void set_err(int e){ win32_set_last_error(g_curc, (uint32_t)e); }
static uint32_t get_err(void){ return win32_get_last_error(g_curc); }
#define FAIL(e) do { set_err(e); return 0xFFFFFFFFu; } while (0)   /* SOCKET_ERROR / INVALID_SOCKET */

int ws_errno_to_wsa(int e){
    switch (e){
    case 0: return 0;
    case EINTR: return WSAEINTR;
    case EBADF: return WSAENOTSOCK;
    case EACCES: return WSAEACCES;
    case EPERM: return WSAEACCES;
    case EFAULT: return WSAEFAULT;
    case EINVAL: return WSAEINVAL;
    case EMFILE: case ENFILE: return WSAEMFILE;
    case EAGAIN: return WSAEWOULDBLOCK;
    case EINPROGRESS: return WSAEWOULDBLOCK;          /* non-blocking connect: Windows says WOULDBLOCK */
    case EALREADY: return WSAEALREADY;
    case ENOTSOCK: return WSAENOTSOCK;
    case EDESTADDRREQ: return WSAEDESTADDRREQ;
    case EMSGSIZE: return WSAEMSGSIZE;
    case EPROTOTYPE: return WSAEPROTOTYPE;
    case ENOPROTOOPT: return WSAENOPROTOOPT;
    case EPROTONOSUPPORT: return WSAEPROTONOSUPPORT;
#ifdef ESOCKTNOSUPPORT
    case ESOCKTNOSUPPORT: return WSAESOCKTNOSUPPORT;
#endif
    case EOPNOTSUPP: return WSAEOPNOTSUPP;
#ifdef EPFNOSUPPORT
    case EPFNOSUPPORT: return WSAEPFNOSUPPORT;
#endif
    case EAFNOSUPPORT: return WSAEAFNOSUPPORT;
    case EADDRINUSE: return WSAEADDRINUSE;
    case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
    case ENETDOWN: return WSAENETDOWN;
    case ENETUNREACH: return WSAENETUNREACH;
    case ENETRESET: return WSAENETRESET;
    case ECONNABORTED: return WSAECONNABORTED;
    case ECONNRESET: return WSAECONNRESET;
    case EPIPE: return WSAECONNRESET;
    case ENOBUFS: return WSAENOBUFS;
    case EISCONN: return WSAEISCONN;
    case ENOTCONN: return WSAENOTCONN;
#ifdef ESHUTDOWN
    case ESHUTDOWN: return WSAESHUTDOWN;
#endif
    case ETIMEDOUT: return WSAETIMEDOUT;
    case ECONNREFUSED: return WSAECONNREFUSED;
#ifdef EHOSTDOWN
    case EHOSTDOWN: return WSAEHOSTDOWN;
#endif
    case EHOSTUNREACH: return WSAEHOSTUNREACH;
    default:
        { static int said; if (said++ < 8) ws_log("unmapped errno %d (%s) -> WSAENETDOWN", e, strerror(e)); }
        return WSAENETDOWN;
    }
}

void ws_fd_setup(int fd){
#ifdef __wasm__
    (void)fd;                             /* virtual fds never block and never signal */
#else
    int fl = fcntl(fd, F_GETFL); if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

/* ======================================================================= */
/* configuration                                                            */
/* ======================================================================= */
static uint32_t g_loop_peer;                          /* loopback: the pseudo peer */
static int g_loop_off;                                /* loopback: own ports + this on the host (NFSU2_NET_LOOPOFFSET) */
static uint16_t lo_host(uint16_t p){ return (uint16_t)(p && g_loop_off ? ((p - 1 + g_loop_off) % 65535) + 1 : p); }
static uint16_t lo_guest(uint16_t h){ return (uint16_t)(h && g_loop_off ? ((h - 1 + 65535 - g_loop_off % 65535) % 65535) + 1 : h); }
static int g_loop_peer_off = -1;                      /* loopback: peer instance's NFSU2_NET_LOOPOFFSET, -1 none */
static uint16_t lo_shift(uint16_t p, int off){ return (uint16_t)(p && off ? ((p - 1 + off) % 65535) + 1 : p); }
static struct { uint16_t p, q; } g_loopmap[16]; static int g_nloopmap;
static int g_harness;
#define DENY_NOTICE_S 20                              /* see deny_note */
static int g_deny_sim;                                /* NFSU2_NET_SIMULATE_DENY: errno, 0 = off */
static uint64_t g_deny_sim_ms, g_deny_sim_t0;
static int g_deny_sim_over;
static uint64_t g_deny_t0, g_deny_said_us;            /* Local Network refusals (deny_note) */
static int g_deny_noticed, g_deny_notice_s, g_deny_strong, g_deny_heard;
static int g_initted;
static char g_vlan_path[512];
static void ident_init(void);

static void parse_loopmap(const char *s){
    g_nloopmap = 0;
    while (s && *s && g_nloopmap < 16){
        unsigned p = 0, q = 0; int n = 0;
        if (sscanf(s, "%u=%u%n", &p, &q, &n) == 2 && p && q && p < 65536 && q < 65536){
            g_loopmap[g_nloopmap].p = (uint16_t)p; g_loopmap[g_nloopmap].q = (uint16_t)q; g_nloopmap++; }
        else { ws_log("NFSU2_NET_LOOPMAP: cannot parse \"%s\"", s); break; }
        s += n; while (*s == ',' || *s == ' ') s++;
    }
}
static int loop_map(uint16_t p){ for (int i = 0; i < g_nloopmap; i++) if (g_loopmap[i].p == p) return g_loopmap[i].q; return 0; }
static int loop_unmap(uint16_t q){ for (int i = 0; i < g_nloopmap; i++) if (g_loopmap[i].q == q) return g_loopmap[i].p; return 0; }

static const char *mode_name(int m){
    return m == NET_OFF ? "off" : m == NET_LAN ? "lan" : m == NET_LOOP ? "loopback" : m == NET_VLAN ? "vlan" : "?";
}
/* For tests (tests/net_harness.c): select a mode before the first call. */
void winsock_test_reset(void);

/* A test or debug run, as opposed to someone playing: the real network is
 * then refused.  The rule is shared with free roam (nettest.h). */
#include "nettest.h"
static int ws_harness(void){ return nfsu2_test_run(); }

/* ---- online: the server's own stand-in at 10.77.0.1 (see the top) ----
 * The lobby host's name and where the exe keeps it are the game's (game.h
 * online_host, online_host_va; UG2: "*pcnfs05.ea.com" at 0x7FBDBC). */
#define ONLINE_LOBBY   20922u                     /* its lobby's port (nfsu2-online PORT_LOBBY) */
void online_lobby_read(uint32_t link, const uint8_t *p, int n);   /* online.c: the persona the server gave */
void olfilter_lobby_mark(uint32_t link, uint8_t *p, int n);       /* olfilter.c: a search with the player's Filter */
void olfilter_lobby_sent(uint32_t link, uint8_t *p, int n);
static int g_online;
static int env_off(const char *v){
    return v && (!strcmp(v, "0") || !strcasecmp(v, "off") || !strcasecmp(v, "no") || !strcasecmp(v, "false"));
}
/* the lobby host string without its '*' (no SSL): guest data, at run time; the file is never touched */
static void online_patch(void){
    const char *host = g_game.online_host; uint32_t va = g_game.online_host_va;
    if (!host || !va){ g_online = 0; return; }             /* the game has no Online lobby we know */
    char secure[64] = "", plain[64] = "";                  /* the strings with their NULs, zero-padded */
    snprintf(secure, sizeof secure, "*%s", host); snprintf(plain, sizeof plain, "%s", host);
    uint32_t n = (uint32_t)strlen(secure) + 1u;           /* UG2: 16 bytes */
    if (!gok(va, n)) return;                               /* not the game (a test harness) */
    if (!memcmp(g_mem + va, plain, n)) return;
    if (memcmp(g_mem + va, secure, n)){
        ws_log("online: the lobby host at 0x%08x is not \"*%s\" (another game version?): online stays off", va, host);
        g_online = 0; return;
    }
    memcpy(g_mem + va, plain, n);
    ws_log("online: on (NFSU2_ONLINE): the lobby link is plain TCP to %s:20920, which is the server's own host 10.77.0.1",
           host);
}
static void ws_config(void){
    if (g_initted) return;
    g_initted = 1;
    g_harness = ws_harness();
    const char *m = getenv("NFSU2_NET");
    int mode = NET_LAN;
    if (m && *m){
        if (!strcasecmp(m, "off") || !strcmp(m, "0") || !strcasecmp(m, "none")) mode = NET_OFF;
        else if (!strcasecmp(m, "lan") || !strcasecmp(m, "on") || !strcmp(m, "1") || !strcasecmp(m, "bsd")) mode = NET_LAN;
        else if (!strcasecmp(m, "loopback") || !strcasecmp(m, "loop") || !strcasecmp(m, "sandbox")) mode = NET_LOOP;
        else if (!strncasecmp(m, "vlan", 4) && (m[4] == 0 || m[4] == ':')){
            mode = NET_VLAN;
            const char *p = m[4] == ':' ? m + 5 : getenv("NFSU2_VLAN");
            snprintf(g_vlan_path, sizeof g_vlan_path, "%s", p ? p : "");
            if (!g_vlan_path[0]){ ws_log("NFSU2_NET=vlan needs a hub socket (vlan:<path> or NFSU2_VLAN): network off"); mode = NET_OFF; }
        } else { ws_log("NFSU2_NET=\"%s\" is not off|lan|loopback|vlan[:path]: network off", m); mode = NET_OFF; }
    }
#ifdef __wasm__
    /* the browser: no real network, only the server's virtual LAN (net_lan.c) */
    if (mode != NET_OFF){
        if (mode != NET_VLAN && m && *m) ws_log("NFSU2_NET=%s: in the browser LAN play is the server's virtual LAN", m);
        mode = NET_VLAN; snprintf(g_vlan_path, sizeof g_vlan_path, "switch");
    }
#endif
    if (mode == NET_LAN && g_harness){
        ws_log("lan mode refused in a test or debug run (NFSU2_NET_HARNESS, an input script, NFSU2_TIMEOUT or ALARM != 0): "
               "using loopback instead; play through the app or ./run.sh for the real LAN");
        mode = NET_LOOP;
    }
    g_ws_mode = mode;
#ifdef __wasm__
    g_online = mode == NET_VLAN && !env_off(getenv("NFSU2_ONLINE"));
#else
    g_online = 0;                                          /* the Mac's hub (vlanhub.py) has no online host */
#endif
    if (g_online) online_patch();
    { const char *d = getenv("NFSU2_NET_SIMULATE_DENY");
      g_deny_sim = 0; g_deny_sim_ms = g_deny_sim_t0 = 0; g_deny_sim_over = 0;
      g_deny_t0 = g_deny_said_us = 0; g_deny_noticed = 0; g_deny_strong = 0; g_deny_heard = 0;
      const char *ns = getenv("NFSU2_NET_DENY_NOTICE_S");   /* tests: seconds before the notice */
      g_deny_notice_s = ns && *ns ? atoi(ns) : DENY_NOTICE_S;
      if (d && *d && *d != '0' && (mode == NET_LOOP || mode == NET_VLAN)){
          g_deny_sim = !strncasecmp(d, "EPERM", 5) ? EPERM : EHOSTUNREACH;
          const char *k = strchr(d, ':'); if (k) g_deny_sim_ms = strtoull(k + 1, 0, 10);
      } }
    if (mode == NET_LOOP){
        const char *pp = getenv("NFSU2_NET_LOOPPEER");
        if (!parse_ip(pp, &g_loop_peer)) g_loop_peer = mkip(10, 77, 0, 3);
        const char *lm = getenv("NFSU2_NET_LOOPMAP");
        parse_loopmap(lm ? lm : "9999=19999,9900=19900,3658=13658");
        const char *lo = getenv("NFSU2_NET_LOOPOFFSET");
        g_loop_off = lo ? atoi(lo) % 65535 : 0;
        if (g_loop_off < 0) g_loop_off += 65535;
        const char *po = getenv("NFSU2_NET_LOOPPEEROFFSET");
        g_loop_peer_off = -1;
        if (po && *po){ g_loop_peer_off = atoi(po) % 65535; if (g_loop_peer_off < 0) g_loop_peer_off += 65535; }
    }
    ident_init();
}

/* ======================================================================= */
/* identity                                                                 */
/* ======================================================================= */
/* 64-bit FNV-1a */
static uint64_t fnv(const void *p, size_t n, uint64_t h){
    const uint8_t *b = p; if (!h) h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++){ h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
static void mac_local_from(uint64_t h, uint8_t mac[6]){
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)(h >> (8 * i));
    mac[0] = (uint8_t)((mac[0] & 0xFC) | 0x02);       /* locally administered, unicast */
    if (!(mac[1] | mac[2] | mac[3] | mac[4] | mac[5])) mac[5] = 1;
}
static int parse_mac(const char *s, uint8_t mac[6]){
    unsigned v[6];
    if (!s || sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return 0;
    int nz = 0;
    for (int i = 0; i < 6; i++){ if (v[i] > 255) return 0; mac[i] = (uint8_t)v[i]; nz |= mac[i]; }
    return nz != 0;
}
static int lan_ifname_ok(const char *n){
    static const char *bad[] = { "lo", "utun", "ipsec", "ppp", "awdl", "llw", "bridge", "ap", "anpi",
                                 "gif", "stf", "nan", "vmenet", "feth", "tun", "tap", 0 };
    for (int i = 0; bad[i]; i++) if (!strncmp(n, bad[i], strlen(bad[i]))) return 0;
    return 1;
}
/* Interface name and index of the default IPv4 route (routing socket
 * RTM_GET: a read of the kernel's routing table, no traffic). */
#ifdef __wasm__
static int route_default_if(char *ifn, size_t ifnsz){ (void)ifn; (void)ifnsz; return 0; }   /* no interfaces in a browser */
#else
static int route_default_if(char *ifn, size_t ifnsz){
    int s = socket(PF_ROUTE, SOCK_RAW, AF_INET);
    if (s < 0) return 0;
    struct { struct rt_msghdr h; uint8_t b[512]; } m;
    memset(&m, 0, sizeof m);
    static int seq = 0x4e46;
    struct sockaddr_in *dst = (struct sockaddr_in *)m.b;
    dst->sin_len = sizeof *dst; dst->sin_family = AF_INET; dst->sin_addr.s_addr = 0;
    struct sockaddr_dl *ifp = (struct sockaddr_dl *)(m.b + sizeof *dst);
    ifp->sdl_len = sizeof *ifp; ifp->sdl_family = AF_LINK;
    m.h.rtm_msglen = (u_short)(sizeof m.h + sizeof *dst + sizeof *ifp);
    m.h.rtm_version = RTM_VERSION; m.h.rtm_type = RTM_GET;
    m.h.rtm_addrs = RTA_DST | RTA_IFP; m.h.rtm_pid = getpid(); m.h.rtm_seq = ++seq;
    int ok = 0;
    if (write(s, &m, m.h.rtm_msglen) > 0){
        struct timeval tv = { 1, 0 }; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        for (int tries = 0; tries < 16 && !ok; tries++){
            ssize_t n = read(s, &m, sizeof m);
            if (n <= 0) break;
            if (m.h.rtm_pid != getpid() || m.h.rtm_seq != seq || m.h.rtm_errno) continue;
            uint8_t *p = m.b, *end = (uint8_t *)&m + n;
            for (int bit = 0; bit < RTAX_MAX && p < end; bit++){
                if (!(m.h.rtm_addrs & (1 << bit))) continue;
                struct sockaddr *sa = (struct sockaddr *)p;
                int l = sa->sa_len ? (1 + ((sa->sa_len - 1) | (int)(sizeof(uint32_t) - 1))) : (int)sizeof(uint32_t);
                if (bit == RTAX_IFP && sa->sa_family == AF_LINK){
                    struct sockaddr_dl *dl = (struct sockaddr_dl *)sa;
                    if (dl->sdl_nlen && dl->sdl_nlen < ifnsz){ memcpy(ifn, dl->sdl_data, dl->sdl_nlen); ifn[dl->sdl_nlen] = 0; ok = 1; }
                    else if (dl->sdl_index && if_indextoname(dl->sdl_index, ifn)) ok = 1;
                }
                p += l;
            }
            break;
        }
    }
    close(s);
    return ok;
}
#endif
/* LAN identity (mode lan).  Exposed for tests/net_harness.c (read-only). */
typedef struct { char name[32]; uint32_t ip, mask; uint8_t mac[6]; int has_mac, index, ll; } IfEnt;
static IfEnt g_ifs[16]; static int g_nifs;
static char g_id_warn[300];             /* a choice the user made that could not be honoured ("" = none) */
/* tests/net_harness.c: a made-up interface list and default route instead of
 * the Mac's (getifaddrs / RTM_GET); n < 0 goes back to the real ones. */
static WsTestIf g_tif[16]; static int g_ntif = -1; static char g_tdflt[IFNAMSIZ + 1];
void winsock_test_ifaces(const WsTestIf *l, int n, const char *dflt){
    g_ntif = n < 0 ? -1 : n > 16 ? 16 : n;
    for (int i = 0; i < g_ntif; i++) g_tif[i] = l[i];
    snprintf(g_tdflt, sizeof g_tdflt, "%s", dflt ? dflt : "");
}
const char *winsock_lan_warning(void){ return g_id_warn; }
static int is_ll(uint32_t ip){ const uint8_t *q = (const uint8_t *)&ip; return q[0] == 169 && q[1] == 254; }
/* Every IPv4 address of an interface that is up, running and broadcast
 * capable (not loopback, not point-to-point), with its mask, interface index
 * and hardware address.  Self-assigned 169.254/16 addresses are kept but
 * marked: the automatic choice takes one only when the Mac has no other LAN
 * address (a cable straight to a PC, or a switch without DHCP). */
static void ifs_collect(void){
    g_nifs = 0;
    if (g_ntif >= 0){
        for (int i = 0; i < g_ntif && g_nifs < 16; i++){
            IfEnt *e = &g_ifs[g_nifs]; memset(e, 0, sizeof *e);
            if (!parse_ip(g_tif[i].ip, &e->ip)) continue;
            if (!parse_ip(g_tif[i].mask, &e->mask)) e->mask = mkip(255, 255, 255, 0);
            snprintf(e->name, sizeof e->name, "%s", g_tif[i].name);
            e->index = 100 + i;
            for (int j = 0; j < i; j++) if (!strcmp(g_tif[j].name, g_tif[i].name)){ e->index = 100 + j; break; }
            e->has_mac = parse_mac(g_tif[i].mac, e->mac);
            e->ll = is_ll(e->ip);
            g_nifs++;
        }
        return;
    }
#ifndef __wasm__
    struct ifaddrs *ia = 0, *p;
    if (getifaddrs(&ia) != 0) ia = 0;
    for (p = ia; p && g_nifs < 16; p = p->ifa_next){
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        unsigned fl = p->ifa_flags;
        if (!(fl & IFF_UP) || !(fl & IFF_RUNNING) || (fl & IFF_LOOPBACK) || (fl & IFF_POINTOPOINT)) continue;
        if (!(fl & IFF_BROADCAST)) continue;
        uint32_t ip = ((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr;
        IfEnt *e = &g_ifs[g_nifs++]; memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", p->ifa_name);
        e->ip = ip; e->ll = is_ll(ip);
        e->mask = p->ifa_netmask ? ((struct sockaddr_in *)p->ifa_netmask)->sin_addr.s_addr : mkip(255, 255, 255, 0);
        e->index = (int)if_nametoindex(p->ifa_name);
    }
    for (p = ia; p; p = p->ifa_next){
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_LINK) continue;
        struct sockaddr_dl *dl = (struct sockaddr_dl *)p->ifa_addr;
        if (dl->sdl_alen != 6) continue;
        for (int i = 0; i < g_nifs; i++)
            if (!strcmp(g_ifs[i].name, p->ifa_name)){ memcpy(g_ifs[i].mac, LLADDR(dl), 6); g_ifs[i].has_mac = 1; }
    }
    if (ia) freeifaddrs(ia);
#endif
}
/* The choice, in this order:
 *   lan_address (NFSU2_NET_ADDR): the interface that holds that address --
 *     its index (broadcasts leave through it, IP_BOUND_IF), mask and MAC
 *     come with it, so discovery, adverts and the server's listener are all
 *     on one network.  An address the Mac does not have is ignored (warning).
 *   lan_interface (NFSU2_NET_IFACE): that interface's address, a
 *     self-assigned one too.  No IPv4 address there: warning, automatic.
 *   automatic: the default route's interface if it is a LAN one, else the
 *     first en*, else any LAN-like one; a self-assigned address only when
 *     there is nothing else. */
int winsock_lan_identity(NetIdent *id, char *why, size_t whysz){
    memset(id, 0, sizeof *id);
    g_id_warn[0] = 0;
    ifs_collect();
    const char *want = getenv("NFSU2_NET_IFACE"), *wa = getenv("NFSU2_NET_ADDR");
    char dflt[IFNAMSIZ + 1] = "";
    int have_dflt = g_ntif >= 0 ? (snprintf(dflt, sizeof dflt, "%s", g_tdflt), g_tdflt[0] != 0)
                                : route_default_if(dflt, sizeof dflt);
    int pick = -1; const char *how = "";
    if (wa && *wa){
        uint32_t v;
        if (!parse_ip(wa, &v))
            snprintf(g_id_warn, sizeof g_id_warn, "lan_address = %s is not an IPv4 address: ignored", wa);
        else {
            for (int i = 0; i < g_nifs && pick < 0; i++) if (g_ifs[i].ip == v) pick = i;
            if (pick < 0) snprintf(g_id_warn, sizeof g_id_warn, "lan_address = %s is not an address of this Mac "
                                   "(not connected, or a typo): ignored, the automatic choice is used", wa);
            else {
                how = ", lan_address";
                if (want && *want && strcmp(want, g_ifs[pick].name))
                    snprintf(g_id_warn, sizeof g_id_warn, "lan_address = %s is on %s, not on lan_interface = %s: "
                             "%s is used", wa, g_ifs[pick].name, want, g_ifs[pick].name);
            }
        }
    }
    if (pick < 0 && want && *want){
        for (int i = 0; i < g_nifs; i++)
            if (!strcmp(g_ifs[i].name, want) && (pick < 0 || (g_ifs[pick].ll && !g_ifs[i].ll))) pick = i;
        if (pick >= 0) how = ", lan_interface";
        else if (!g_id_warn[0])
            snprintf(g_id_warn, sizeof g_id_warn, "lan_interface = %s has no IPv4 address (not connected, or no such "
                     "interface): the automatic choice is used, and LAN games on %s's network will not be found", want, want);
    }
    if (pick < 0 && have_dflt && lan_ifname_ok(dflt))
        for (int i = 0; i < g_nifs; i++) if (!g_ifs[i].ll && !strcmp(g_ifs[i].name, dflt)){ pick = i; break; }
    if (pick < 0) for (int i = 0; i < g_nifs; i++) if (!g_ifs[i].ll && !strncmp(g_ifs[i].name, "en", 2)){ pick = i; break; }
    if (pick < 0) for (int i = 0; i < g_nifs; i++) if (!g_ifs[i].ll && lan_ifname_ok(g_ifs[i].name)){ pick = i; break; }
    if (pick < 0) for (int i = 0; i < g_nifs; i++) if (g_ifs[i].ll && lan_ifname_ok(g_ifs[i].name)){ pick = i; break; }
    if (pick >= 0){
        IfEnt *e = &g_ifs[pick];
        id->ip = e->ip; id->mask = e->mask; id->ifindex = e->index;
        snprintf(id->ifname, sizeof id->ifname, "%s", e->name);
        if (e->has_mac && !(e->mac[0] & 2) && (e->mac[0] | e->mac[1] | e->mac[2] | e->mac[3] | e->mac[4] | e->mac[5]))
            memcpy(id->mac, e->mac, 6);
        snprintf(why, whysz, "%s%s%s%s", e->name, have_dflt && !strcmp(dflt, e->name) ? " (default route)" : "", how,
                 e->ll ? ", self-assigned" : "");
    } else snprintf(why, whysz, "no LAN interface with an IPv4 address%s%s",
                    have_dflt ? "; default route via " : "", have_dflt ? dflt : "");
    if (!(id->mac[0] | id->mac[1] | id->mac[2] | id->mac[3] | id->mac[4] | id->mac[5])){
#ifdef __wasm__
        uint64_t hsh = 0;
        { char hn[256] = ""; gethostname(hn, sizeof hn); hsh = fnv(hn, strlen(hn), fnv("NFSU2-MAC", 9, 0)); }
#else
        uuid_t u; struct timespec to = { 1, 0 };
        uint64_t hsh = 0;
        if (gethostuuid(u, &to) == 0) hsh = fnv(u, sizeof(uuid_t), fnv("NFSU2-MAC", 9, 0));
        else { char hn[256] = ""; gethostname(hn, sizeof hn); hsh = fnv(hn, strlen(hn), fnv("NFSU2-MAC", 9, 0)); }
#endif
        mac_local_from(hsh, id->mac);
    }
    { const char *em = getenv("NFSU2_NET_MAC"); uint8_t m[6];
      if (em && *em){ if (parse_mac(em, m)) memcpy(id->mac, m, 6); else ws_log("NFSU2_NET_MAC=%s ignored (xx:xx:xx:xx:xx:xx, not all zero)", em); } }
    { char hn[256] = "";
      const char *en = getenv("NFSU2_NET_NAME");
      if (en && *en) snprintf(hn, sizeof hn, "%s", en); else gethostname(hn, sizeof hn);
      char *dot = strchr(hn, '.'); if (dot) *dot = 0;
      int k = 0;
      for (char *s = hn; *s && k < 63; s++){
          char ch = *s;
          if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-')) ch = '-';
          id->name[k++] = ch;
      }
      id->name[k] = 0;
      if (!k) snprintf(id->name, sizeof id->name, "NFSU2-MAC"); }
    return id->ip != 0;
}
static void ident_virtual(NetIdent *id, uint32_t ip, const char *name){
    memset(id, 0, sizeof *id);
    id->ip = ip; id->mask = mkip(255, 255, 255, 0);
    const uint8_t *q = (const uint8_t *)&ip;
    id->mac[0] = 0x02; id->mac[1] = 0x4E; id->mac[2] = 0x56; id->mac[3] = 0x4C; id->mac[4] = q[2]; id->mac[5] = q[3];
    snprintf(id->name, sizeof id->name, "%s", name);
    snprintf(id->ifname, sizeof id->ifname, "vlan0");
    const char *em = getenv("NFSU2_NET_MAC"); uint8_t m[6];
    if (em && *em && parse_mac(em, m)) memcpy(id->mac, m, 6);
    const char *en = getenv("NFSU2_NET_NAME");
    if (en && *en) snprintf(id->name, sizeof id->name, "%s", en);
}
/* lan: (re)computes the identity.  Windows answers the route query from the
 * routing table of the moment; here the answer is taken when a network
 * session starts (the game's WSAStartup when it enters its LAN screens,
 * ws_startup), so a network change takes effect once the player leaves LAN
 * and comes back.  Within a session it stays fixed: the game's sockets,
 * server.dll's listener and the adverts are bound to it. */
static int g_lan_fresh;                 /* identity computed since the last session ended */
static void ident_lan(void){
    char b[16], why[200] = "";
    int ok = winsock_lan_identity(&g_ws_id, why, sizeof why);
    if (g_id_warn[0]) ws_log("WARNING: %s", g_id_warn);
    ws_log("mode lan: address %s (%s), host name %s, adapter %02x:%02x:%02x:%02x:%02x:%02x%s",
           ok ? ipstr(g_ws_id.ip, b) : "none", why, g_ws_id.name,
           g_ws_id.mac[0], g_ws_id.mac[1], g_ws_id.mac[2], g_ws_id.mac[3], g_ws_id.mac[4], g_ws_id.mac[5],
           ok ? "" : " -- LAN play needs a network connection");
    g_lan_fresh = 1;
}
static void ident_init(void){
    char b[16];
    if (g_ws_mode == NET_LAN){
        ident_lan();
    } else if (g_ws_mode == NET_LOOP){
        uint32_t v; const char *wa = getenv("NFSU2_NET_ADDR");
        if (!parse_ip(wa, &v) || is_loop(v)) v = mkip(10, 77, 0, 2);
        ident_virtual(&g_ws_id, v, "NFSU2-LOOP");
        char pb[16];
        ws_log("mode loopback: guest address %s, peer %s, host name %s, adapter %02x:%02x:%02x:%02x:%02x:%02x; "
               "every host socket on 127.0.0.1 only", ipstr(v, b), ipstr(g_loop_peer, pb), g_ws_id.name,
               g_ws_id.mac[0], g_ws_id.mac[1], g_ws_id.mac[2], g_ws_id.mac[3], g_ws_id.mac[4], g_ws_id.mac[5]);
        if (g_loop_peer_off >= 0)
            ws_log("loopback peer %s is a second instance: own ports +%d, its ports +%d on 127.0.0.1",
                   ipstr(g_loop_peer, pb), g_loop_off, g_loop_peer_off);
    } else if (g_ws_mode == NET_VLAN){
        /* filled in by the hub (HELLO) at the first WSAStartup */
        ident_virtual(&g_ws_id, 0, "VLAN");
    } else {
        ident_virtual(&g_ws_id, mkip(10, 77, 0, 1), "NFSU2-OFF");
        ws_log("mode off: no network (WSAStartup fails with WSASYSNOTREADY)");
    }
}
static int g_vlan_up;           /* HELLO done */
static int vlan_up(void){
    if (g_vlan_up) return vlan_alive() ? 0 : WSAENETDOWN;
    int e = vlan_hello(g_vlan_path, &g_ws_id);
    if (e){ ws_log("vlan: hub %s not reachable (WSA %d): network down", g_vlan_path, e); return e; }
    g_vlan_up = 1;
    char b[16];
    ws_log("mode vlan: hub %s, address %s, host name %s, adapter %02x:%02x:%02x:%02x:%02x:%02x",
           g_vlan_path, ipstr(g_ws_id.ip, b), g_ws_id.name, g_ws_id.mac[0], g_ws_id.mac[1], g_ws_id.mac[2],
           g_ws_id.mac[3], g_ws_id.mac[4], g_ws_id.mac[5]);
    return 0;
}

/* Which local address would be used to reach `dst` (SIO_ROUTING_INTERFACE_QUERY). */
static int route_query(uint32_t dst, uint32_t *out){
    if (is_loop(dst)){ *out = mkip(127, 0, 0, 1); return 0; }
    if (g_ws_mode != NET_LAN){ *out = g_ws_id.ip; return g_ws_id.ip ? 0 : WSAENETUNREACH; }
    if (dst == g_ws_id.ip){ *out = dst; return 0; }
    for (int i = 0; i < g_nifs; i++) if (dst == g_ifs[i].ip){ *out = dst; return 0; }   /* another local address */
    /* on the chosen interface's subnet, else on another interface's (a
     * subnet two adapters share is answered with the chosen one) */
    if (g_ws_id.ip && ((dst ^ g_ws_id.ip) & g_ws_id.mask) == 0){ *out = g_ws_id.ip; return 0; }
    for (int i = 0; i < g_nifs; i++)
        if (g_ifs[i].ip != g_ws_id.ip && ((dst ^ g_ifs[i].ip) & g_ifs[i].mask) == 0){ *out = g_ifs[i].ip; return 0; }
    if (!g_ws_id.ip) return WSAENETUNREACH;
    *out = g_ws_id.ip; return 0;
}
/* tests/net_harness.c: the route query against the lan identity of the
 * current (made-up) interface list, without lan mode: no socket, no packet. */
int winsock_test_route(uint32_t dst, uint32_t *out){
    int m = g_ws_mode; NetIdent keep = g_ws_id; char why[200] = "";
    winsock_lan_identity(&g_ws_id, why, sizeof why);
    g_ws_mode = NET_LAN;
    int e = route_query(dst, out);
    g_ws_mode = m; g_ws_id = keep;
    return e;
}
static int is_bcast(uint32_t ip){
    if (ip == 0xFFFFFFFFu) return 1;
    if (g_ws_id.mask && g_ws_id.ip && ip == (g_ws_id.ip | ~g_ws_id.mask)) return 1;
    return 0;
}

/* ======================================================================= */
/* the SOCKET table                                                         */
/* ======================================================================= */
#define NSOCK 1024
#define SOCK_BASE 0x1000u
static WsSock g_s[NSOCK];
static int g_s_next = 1;
static int g_startups;                    /* WSAStartup reference count */

static WsSock *sget(uint32_t h){
    if (h < SOCK_BASE + 4u || (h - SOCK_BASE) & 3u) return 0;
    uint32_t i = (h - SOCK_BASE) >> 2;
    if (i >= NSOCK || !g_s[i].used || atomic_load(&g_s[i].closing)) return 0;
    return &g_s[i];
}
static uint32_t shandle(WsSock *s){ return SOCK_BASE + 4u * (uint32_t)(s - g_s); }
static WsSock *salloc(void){
    for (int k = 0; k < NSOCK - 1; k++){
        int i = g_s_next + k; if (i >= NSOCK) i -= NSOCK - 1;
        WsSock *s = &g_s[i];
        if (s->used) continue;
        memset(s, 0, sizeof *s);
        s->used = 1; s->fd = -1; s->ip_ttl = 128; s->sndbuf = s->rcvbuf = 65536;
        g_s_next = i + 1 >= NSOCK ? 1 : i + 1;          /* round robin: no immediate handle reuse */
        st_sockets++;
        return s;
    }
    return 0;
}
static void sfinal(WsSock *s){                         /* last reference gone */
    if (s->fd >= 0) close(s->fd);
    if (s->aux_fd > 0) close(s->aux_fd);
    s->fd = -1; s->aux_fd = 0; s->used = 0; atomic_store(&s->closing, 0);
}
static void sclose(WsSock *s, int abortive){
    if (g_ws_mode == NET_VLAN) vlan_close(s, abortive);
#ifndef __wasm__
    else if (s->fd >= 0 && s->type == 1 && abortive){
        struct linger l = { 1, 0 }; setsockopt(s->fd, SOL_SOCKET, SO_LINGER, &l, sizeof l);
    }
#endif
    if (s->waiters){
        atomic_store(&s->closing, 1);
        if (s->fd >= 0) shutdown(s->fd, SHUT_RDWR);    /* wakes TCP waiters at once, the rest within 50 ms */
    } else sfinal(s);
}

/* ======================================================================= */
/* waiting with the guest lock released                                     */
/* ======================================================================= */
/* 0 = ready, 1 = deadline passed, 2 = the socket was closed meanwhile */
static int swait(WsSock *s, short ev, uint64_t deadline){
    int fd = s->fd, rc = 1;
    s->waiters++; st_waits++;
    guest_wait_begin();
    for (;;){
        if (atomic_load(&s->closing)){ rc = 2; break; }
        uint64_t now = ws_now_us();
        if (deadline && now >= deadline){ rc = 1; break; }
        int ms = 50;
        if (deadline && (deadline - now) / 1000 < 50) ms = (int)((deadline - now + 999) / 1000);
        if (fd < 0){ struct timespec ts = { 0, (long)ms * 1000000L }; nanosleep(&ts, 0); continue; }
        struct pollfd p = { fd, ev, 0 };
        int n = poll(&p, 1, ms);
        if (n > 0){ rc = atomic_load(&s->closing) ? 2 : 0; break; }
    }
    guest_wait_end();
    s->waiters--;
    if (atomic_load(&s->closing)){ if (!s->waiters) sfinal(s); return 2; }
    return rc;
}
static uint64_t deadline_ms(uint32_t ms){ return ms ? ws_now_us() + (uint64_t)ms * 1000ull : 0; }

/* ======================================================================= */
/* address translation and the network policy                               */
/* ======================================================================= */
/* Guest SOCKADDR_IN: u16 family, u16 port (big endian), u32 address (network
 * order), 8 bytes sin_zero (ignored on input: DirtySock keeps a TTL and a
 * receive time stamp there). */
static int ga_in(uint32_t p, uint32_t len, uint32_t *ip, uint16_t *port){
    if (!p || len < 16u || !gok(p, 16)) return WSAEFAULT;
    if (MR16(p) != 2) return WSAEAFNOSUPPORT;
    *port = (uint16_t)(MR8(p + 2) << 8 | MR8(p + 3));
    *ip = MR32(p + 4);
    return 0;
}
static int ga_out(uint32_t p, uint32_t plen, uint32_t ip, uint16_t port){
    if (!p) return 0;                                   /* optional out-address */
    if (!plen || !gok(plen, 4)) return WSAEFAULT;
    if ((int32_t)MR32(plen) < 16 || !gok(p, 16)) return WSAEFAULT;
    MR16(p) = 2; MR8(p + 2) = (uint8_t)(port >> 8); MR8(p + 3) = (uint8_t)port;
    MR32(p + 4) = ip; MR32(p + 8) = 0; MR32(p + 12) = 0;
    MR32(plen) = 16;
    return 0;
}
static void blocked(const char *op, uint32_t ip, uint16_t port, CPU *c){
    char b[16];
    st_blocked++;
    if (st_blocked <= 200)
        ws_log("BLOCKED %s %s:%u (mode %s) from 0x%08x", op, ipstr(ip, b), port, mode_name(g_ws_mode), c ? c->eip : 0u);
    else if (st_blocked == 201) ws_log("BLOCKED: further lines suppressed (count in the exit report)");
}
/* ---- Local Network privacy -------------------------------------------------
 * macOS keeps an app off the LAN until the user allows it (System Settings >
 * Privacy & Security > Local Network; the system asks once).  How a refusal
 * reaches BSD sockets is not documented: EHOSTUNREACH ("No route to host") or
 * EPERM / EACCES.  A switched-off peer or a stale address gives the same
 * errors, so a failure is only a hint in the log (again after a minute).  A
 * broadcast or a connect that fails at once is the strong sign; when nothing
 * sent beyond this Mac has worked for DENY_NOTICE_S seconds since the first
 * failure, the app tells the player once (app_local_network_notice in
 * bundle.m: an alert with a button to that settings pane; only a log line in
 * test runs).  The delay keeps it away from the system's own question, during
 * which sends fail too.  The limit is checked after every Winsock call
 * (deny_tick), not only at the next failure: a joiner sends its server-list
 * query once, when it opens the list, and then only listens, so no second
 * failure would ever come (before: no notice in 100 s).
 * A refusal while the system's question is up, answered Allow, must not
 * bring the notice 20 s later: the joiner's one query failed, but access
 * works now.  So when the limit is reached
 * the refused broadcast is sent once more (deny_probe: the same datagram, on
 * the same socket, as if the player pressed refresh); if that works there
 * is no notice, if it is refused again deny_note raises it.  Without a
 * datagram to send again (its socket closed) the check stays quiet once
 * something has arrived from another machine since the refusal
 * (deny_heard).  A TCP connection to another machine, or a send on one,
 * counts as working (deny_ok), and every network session (the first
 * WSAStartup after the last WSACleanup) starts with a clean slate: a
 * refusal on one LAN visit says nothing about the next. */
void app_local_network_notice(void) __attribute__((weak));
static const char *deny_str(int e){ return e == EHOSTUNREACH ? "No route to host" : strerror(e); }
static struct { uint32_t h, ip; uint16_t port; int len; uint8_t buf[1472]; } g_probe;  /* deny_probe */
static int g_probing;
static int send_one(WsSock *s, const uint8_t *buf, int len, int wflags, uint32_t ip, uint16_t port, int has_addr, int *sent, CPU *c);
/* send_one: a broadcast was refused; keep it for deny_tick's second try */
static void deny_keep(WsSock *s, const uint8_t *buf, int len, uint32_t ip, uint16_t port){
    if (g_probing || len < 0 || len > (int)sizeof g_probe.buf) return;
    g_probe.h = shandle(s); g_probe.ip = ip; g_probe.port = port; g_probe.len = len;
    if (len) memcpy(g_probe.buf, buf, (size_t)len);
}
static void deny_note(int e, int strong){
    uint64_t now = ws_now_us();
    if (!g_deny_t0) g_deny_t0 = now;
    if (strong){ g_deny_strong = 1; g_deny_heard = 0; }
    if (!g_deny_said_us || now - g_deny_said_us > 60000000ull){
        g_deny_said_us = now;
        ws_log("LAN traffic refused (%s).  If macOS asked whether NFSU2 may find devices on your local network and "
               "the answer was no, allow NFSU2 in System Settings > Privacy & Security > Local Network.  (A peer that "
               "is switched off or unreachable gives the same error.)", deny_str(e));
    }
    if (strong && !g_deny_noticed && now - g_deny_t0 >= (uint64_t)g_deny_notice_s * 1000000ull){
        g_deny_noticed = 1;
        ws_log("nothing sent beyond this Mac has worked for %d s (%s): macOS is most likely refusing NFSU2 local "
               "network access", (int)((now - g_deny_t0) / 1000000ull), deny_str(e));
        if (app_local_network_notice) app_local_network_notice();
    }
}
/* every Winsock call: a strong refusal seen and nothing beyond this Mac has
 * worked since, DENY_NOTICE_S after the first refusal -> the notice */
static void deny_tick(void){
    if (!g_startups || !g_deny_t0 || !g_deny_strong || g_deny_noticed || g_probing) return;
    uint64_t now = ws_now_us();
    if (now - g_deny_t0 < (uint64_t)g_deny_notice_s * 1000000ull) return;
    WsSock *ps = g_probe.h ? sget(g_probe.h) : 0;
    if (ps && ps->type == 2){
        uint8_t b[sizeof g_probe.buf]; int n = g_probe.len, sent = 0; char a[16];
        memcpy(b, g_probe.buf, (size_t)n); g_probe.h = 0;
        ws_log("Local Network check after %d s: sending the refused datagram again (%d bytes to %s:%u)",
               (int)((now - g_deny_t0) / 1000000ull), n, ipstr(g_probe.ip, a), g_probe.port);
        g_probing = 1;
        int e = send_one(ps, b, n, 0, g_probe.ip, g_probe.port, 1, &sent, g_curc);
        g_probing = 0;
        if (!e || g_deny_noticed) return;            /* it went out (deny_ok), or refused again (deny_note: the notice) */
    }
    g_probe.h = 0;
    if (g_deny_heard) return;
    g_deny_noticed = 1;
    ws_log("nothing sent beyond this Mac has worked for %d s: macOS is most likely refusing NFSU2 local "
           "network access (no traffic since; checked while the game waits)", (int)((now - g_deny_t0) / 1000000ull));
    if (app_local_network_notice) app_local_network_notice();
}
/* something arrived from another machine (ip: its guest address) */
static void deny_heard(uint32_t ip){
    if (!g_deny_strong || g_deny_heard || ip == g_ws_id.ip || is_loop(ip) || !ip) return;
    g_deny_heard = 1;
    char b[16];
    ws_log("LAN traffic from %s arrives after the refusal: no Local Network notice unless a send fails again", ipstr(ip, b));
}
/* a new network session: earlier refusals no longer count */
static void deny_reset(void){
    g_deny_t0 = g_deny_said_us = 0; g_deny_noticed = 0; g_deny_strong = 0; g_deny_heard = 0; g_probe.h = 0;
}
/* a send beyond this Mac worked */
static void deny_ok(void){
    g_deny_strong = 0; g_deny_heard = 0; g_probe.h = 0;
    if (!g_deny_t0) return;
    uint64_t d = ws_now_us() - g_deny_t0;
    if (d > 5000000ull) ws_log("LAN traffic works again (after %d s of refusals)", (int)(d / 1000000ull));
    g_deny_t0 = 0;
}
/* NFSU2_NET_SIMULATE_DENY (tests; loopback and vlan modes) = 1 | EHOSTUNREACH
 * | EPERM, optionally ":<ms>": every UDP / ICMP send and TCP connect to an
 * address other than this instance's own fails with that error from the
 * first such attempt on -- for good, or for <ms> ms (a prompt answered late).
 * Receiving is not touched (whether macOS gates it is not known). */
static int deny_sim(uint32_t ip){
    if (!g_deny_sim || g_deny_sim_over || ip == g_ws_id.ip || is_loop(ip)) return 0;
    uint64_t now = ws_now_us() / 1000ull;
    if (!g_deny_sim_t0){
        g_deny_sim_t0 = now;
        ws_log("simulated Local Network denial (%s) starts%s", deny_str(g_deny_sim), g_deny_sim_ms ? "" : ", for good");
    }
    if (g_deny_sim_ms && now - g_deny_sim_t0 >= g_deny_sim_ms){
        g_deny_sim_over = 1;
        ws_log("simulated Local Network denial over after %llu ms (access allowed)", (unsigned long long)g_deny_sim_ms);
        return 0;
    }
    return g_deny_sim;
}

#ifndef __wasm__              /* BSD sockets (lan, loopback): not in a browser */
/* Loopback guard: guest destination -> host loopback destinations.
 * Returns the number of host destinations (0 = BLOCKED, the error in *err). */
static int loop_dest(uint32_t ip, uint16_t port, int udp, struct sockaddr_in out[2], int *err){
    memset(out, 0, 2 * sizeof out[0]);
    for (int i = 0; i < 2; i++){ out[i].sin_len = sizeof out[i]; out[i].sin_family = AF_INET; out[i].sin_addr.s_addr = htonl(INADDR_LOOPBACK); }
    if (ip == g_ws_id.ip || is_loop(ip)){ out[0].sin_port = htons(lo_host(port)); return 1; }
    if (ip == g_loop_peer && g_loop_peer_off >= 0){                     /* a second instance */
        out[0].sin_port = htons(lo_shift(port, g_loop_peer_off)); return 1;
    }
    if (ip == g_loop_peer){
        int q = loop_map(port);
        if (!q){ *err = udp ? 0 : WSAECONNREFUSED; return 0; }       /* nobody listens there */
        out[0].sin_port = htons((uint16_t)q); return 1;
    }
    if (udp && is_bcast(ip)){
        int n = 0; out[n++].sin_port = htons(lo_host(port));           /* own copy, as on Windows */
        int q = g_loop_peer_off >= 0 ? lo_shift(port, g_loop_peer_off) : loop_map(port);
        if (q && q != lo_host(port)) out[n++].sin_port = htons((uint16_t)q);
        return n;
    }
    *err = WSAENETUNREACH; return -1;
}
/* host source address -> what the guest sees */
/* loopback with a peer instance: is host port h one of this instance's own
 * sockets?  (Ephemeral ports are the kernel's choice, so the offsets alone
 * cannot tell the two instances apart.) */
static int own_host_port(uint16_t h){
    if (!h) return 1;
    for (int i = 1; i < NSOCK; i++){
        WsSock *s = &g_s[i];
        if (!s->used || s->fd < 0) continue;
        struct sockaddr_in a; socklen_t l = sizeof a; memset(&a, 0, sizeof a);
        if (getsockname(s->fd, (struct sockaddr *)&a, &l) == 0 && ntohs(a.sin_port) == h) return 1;
    }
    return 0;
}
static void host_to_guest(const struct sockaddr_in *h, uint32_t *ip, uint16_t *port){
    *ip = h->sin_addr.s_addr; *port = ntohs(h->sin_port);
    if (g_ws_mode == NET_LOOP && is_loop(*ip) && g_loop_peer_off >= 0){
        if (own_host_port(*port)){ *ip = g_ws_id.ip; *port = lo_guest(*port); }
        else { *ip = g_loop_peer; *port = lo_shift(*port, (65535 - g_loop_peer_off) % 65535); }
        return;
    }
    if (g_ws_mode == NET_LOOP && is_loop(*ip)){
        int p = loop_unmap(*port);
        if (p){ *ip = g_loop_peer; *port = (uint16_t)p; }
        else { *ip = g_ws_id.ip; *port = lo_guest(*port); }
    }
}
#endif

/* ======================================================================= */
/* the calls                                                                */
/* ======================================================================= */
static int need_startup(void){ if (!g_startups){ set_err(WSANOTINITIALISED); return 1; } return 0; }
/* lifecycle (win32_ext.c GetMessageA): a LAN session is open -- from the
 * game's WSAStartup on its LAN screens (and server.dll's while it hosts) to
 * the last WSACleanup.  Single-player never calls WSAStartup. */
int winsock_session_active(void){ return g_startups > 0; }
/* online.c: is the game's Online menu served (the browser's vlan mode with
 * NFSU2_ONLINE on, and the right lobby host string)?  Asked before the game's
 * first Winsock call, so it reads the configuration itself. */
int winsock_online(void){
#ifdef __wasm__
    ws_config();
    return g_online;
#else
    return 0;
#endif
}

static void write_wsadata(uint32_t p, uint16_t ver){
    memset(g_mem + p, 0, 400);
    MR16(p) = ver; MR16(p + 2) = 0x0202;
    strcpy((char *)g_mem + p + 4, "WinSock 2.0");
    strcpy((char *)g_mem + p + 4 + 257, "Running");
    MR16(p + 390) = 0; MR16(p + 392) = 0; MR32(p + 396) = 0;   /* iMaxSockets/iMaxUdpDg unused in 2.x */
}
static uint32_t ws_startup(CPU *c){
    uint32_t req = A(c, 0) & 0xFFFF, p = A(c, 1);
    uint8_t maj = (uint8_t)req, min = (uint8_t)(req >> 8);
    if (g_ws_mode == NET_OFF) return WSASYSNOTREADY;
    if (!maj) return WSAVERNOTSUPPORTED;
    if (!gok(p, 400)) return WSAEFAULT;
    if (g_ws_mode == NET_VLAN){ int e = vlan_up(); if (e) return WSASYSNOTREADY; }
    uint16_t ver = (maj > 2 || (maj == 2 && min >= 2)) ? 0x0202 : (uint16_t)req;
    write_wsadata(p, ver);
    if (g_ws_mode == NET_LAN && !g_startups){       /* a session starts: the network of now */
        if (!g_lan_fresh) ident_lan();
        g_lan_fresh = 0;
    }
    if (!g_startups) deny_reset();                  /* ... and no refusal of an earlier one (deny_tick) */
    g_startups++;
    if (g_startups == 1){ static int n; if (n++ < 4) ws_log("WSAStartup(%u.%u) -> %u.%u, mode %s", maj, min, ver & 0xFF, ver >> 8, mode_name(g_ws_mode)); }
    return 0;
}
static uint32_t ws_cleanup(void){
    if (need_startup()) return 0xFFFFFFFFu;
    if (--g_startups == 0){
        int n = 0;
        for (int i = 1; i < NSOCK; i++) if (g_s[i].used && !atomic_load(&g_s[i].closing)){ sclose(&g_s[i], 0); n++; }
        if (n) ws_log("WSACleanup closed %d socket(s)", n);
        if (g_ws_mode == NET_VLAN && g_vlan_up) vlan_session_end();
    }
    return 0;
}

static uint32_t ws_socket(CPU *c){
    int af = (int)A(c, 0), type = (int)A(c, 1), proto = (int)A(c, 2);
    if (need_startup()) return 0xFFFFFFFFu;
#ifdef __wasm__
    if (af != 2) FAIL(WSAEAFNOSUPPORT);                /* Windows' AF_INET (wasi-libc's is 1) */
#else
    if (af != AF_INET) FAIL(WSAEAFNOSUPPORT);
#endif
    if (type == 1){ if (proto && proto != 6) FAIL(WSAEPROTONOSUPPORT); }
    else if (type == 2){ if (proto && proto != 17) FAIL(WSAEPROTONOSUPPORT); }
    else if (type == 3){ if (proto != 1) FAIL(WSAEPROTONOSUPPORT); }   /* raw: ICMP only (ProtoPing) */
    else FAIL(WSAESOCKTNOSUPPORT);
    WsSock *s = salloc();
    if (!s) FAIL(WSAEMFILE);
    s->type = type; s->proto = type == 1 ? 6 : type == 2 ? 17 : 1;
    if (g_ws_mode == NET_VLAN){
        int e = vlan_open(s);
        if (e){ s->used = 0; FAIL(e); }
    }
#ifndef __wasm__
    else {
        int fd = -1;
        if (type == 3){
            /* Windows raw ICMP needs administrator rights; macOS lets anyone
             * have an ICMP datagram socket (echo only, IP header delivered on
             * receive, the identifier kept).  If even that fails, a socket that
             * never receives: pings time out, the lobby still starts. */
            fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
            if (fd < 0){ int sv[2];
                if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0){ ws_fd_setup(sv[1]); s->aux_fd = sv[1]; fd = sv[0]; s->icmp_fake = 1; } }
        } else fd = socket(AF_INET, type == 1 ? SOCK_STREAM : SOCK_DGRAM, 0);
        if (fd < 0){ int e = errno; s->used = 0; FAIL(ws_errno_to_wsa(e)); }
        ws_fd_setup(fd);
        s->fd = fd;
    }
#endif
    return shandle(s);
}

static uint32_t ws_closesocket(CPU *c){
    if (need_startup()) return 0xFFFFFFFFu;
    WsSock *s = sget(A(c, 0)); if (!s) FAIL(WSAENOTSOCK);
    if (g_probe.h == A(c, 0)) g_probe.h = 0;          /* deny_tick: no second try on another socket */
    sclose(s, s->linger_on && !s->linger_s);
    return 0;
}

/* host bind for the BSD backends */
#ifdef __wasm__
static int bsd_bind(WsSock *s, uint32_t ip, uint16_t port){ (void)s; (void)ip; (void)port; return WSAENETDOWN; }
static void bsd_local(WsSock *s, uint32_t *ip, uint16_t *port){ *ip = s->laddr; *port = s->lport; }
#else
static int bsd_bind(WsSock *s, uint32_t ip, uint16_t port){
    struct sockaddr_in h; memset(&h, 0, sizeof h); h.sin_len = sizeof h; h.sin_family = AF_INET; h.sin_port = htons(port);
    if (g_ws_mode == NET_LOOP){
        if (ip && ip != g_ws_id.ip && !is_loop(ip)) return WSAEADDRNOTAVAIL;
        h.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        h.sin_port = htons(lo_host(port));
    } else h.sin_addr.s_addr = ip;
    if (s->icmp_fake) return 0;
    if (bind(s->fd, (struct sockaddr *)&h, sizeof h) != 0) return ws_errno_to_wsa(errno);
    return 0;
}
/* the local address as the guest sees it */
static void bsd_local(WsSock *s, uint32_t *ip, uint16_t *port){
    struct sockaddr_in h; socklen_t l = sizeof h; memset(&h, 0, sizeof h);
    if (s->icmp_fake || getsockname(s->fd, (struct sockaddr *)&h, &l) != 0){ *ip = s->laddr; *port = s->lport; return; }
    *ip = h.sin_addr.s_addr; *port = ntohs(h.sin_port);
    if (g_ws_mode == NET_LOOP){
        *port = lo_guest(*port);
        /* a wildcard bind stays 0.0.0.0 until the socket is connected */
        if (!s->laddr && s->state != ST_CONNECTED && !s->has_peer) *ip = 0; else *ip = g_ws_id.ip;
    }
}
#endif
static int implicit_bind(WsSock *s){
    if (s->bound) return 0;
    int e = g_ws_mode == NET_VLAN ? vlan_bind(s, 0, 0) : bsd_bind(s, 0, 0);
    if (!e){ s->bound = 1; if (s->state == ST_NEW) s->state = ST_BOUND; }
    return e;
}

static uint32_t ws_bind(CPU *c){
    if (need_startup()) return 0xFFFFFFFFu;
    WsSock *s = sget(A(c, 0)); if (!s) FAIL(WSAENOTSOCK);
    uint32_t ip; uint16_t port;
    int e = ga_in(A(c, 1), A(c, 2), &ip, &port); if (e) FAIL(e);
    if (s->bound) FAIL(WSAEINVAL);
    if (g_ws_mode == NET_LAN && ip && !is_loop(ip)){
        int local = ip == g_ws_id.ip;
        for (int i = 0; i < g_nifs && !local; i++) local = g_ifs[i].ip == ip;
        if (!local){ char b[16]; ws_log("bind to %s: not an address of this Mac", ipstr(ip, b)); }
    }
    if (g_ws_mode == NET_LOOP && ip && ip != g_ws_id.ip && !is_loop(ip)){ blocked("bind", ip, port, c); FAIL(WSAEADDRNOTAVAIL); }
    e = g_ws_mode == NET_VLAN ? vlan_bind(s, ip, port) : bsd_bind(s, ip, port);
    if (e) FAIL(e);
    s->bound = 1; s->laddr = ip; if (g_ws_mode != NET_VLAN) s->lport = port;
    if (s->state == ST_NEW) s->state = ST_BOUND;
    return 0;
}

static uint32_t ws_listen(CPU *c){
    if (need_startup()) return 0xFFFFFFFFu;
    WsSock *s = sget(A(c, 0)); if (!s) FAIL(WSAENOTSOCK);
    int bl = (int)A(c, 1);
    if (s->type != 1) FAIL(WSAEOPNOTSUPP);
    if (!s->bound) FAIL(WSAEINVAL);
    if (s->state == ST_CONNECTED || s->state == ST_CONNECTING) FAIL(WSAEISCONN);
    if (s->state == ST_LISTEN) return 0;
    if (bl <= 0) bl = 1; if (bl > 128) bl = 128;
    int e = 0;
    if (g_ws_mode == NET_VLAN) e = vlan_listen(s, bl);
#ifndef __wasm__
    else if (listen(s->fd, bl) != 0) e = ws_errno_to_wsa(errno);
#endif
    if (e) FAIL(e);
    s->state = ST_LISTEN;
    return 0;
}

static uint32_t ws_accept(CPU *c){
    if (need_startup()) return 0xFFFFFFFFu;
    WsSock *ls = sget(A(c, 0)); if (!ls) FAIL(WSAENOTSOCK);
    uint32_t pa = A(c, 1), pl = A(c, 2);
    if (ls->state != ST_LISTEN) FAIL(WSAEINVAL);
    if (pa && (!pl || !gok(pl, 4) || (int32_t)MR32(pl) < 16)) FAIL(WSAEFAULT);
    uint32_t h = shandle(ls);
    for (;;){
        WsSock *ns = salloc(); if (!ns) FAIL(WSAEMFILE);
        ns->type = 1; ns->proto = 6;
        int e = 0; uint32_t ip = 0; uint16_t port = 0;
        if (g_ws_mode == NET_VLAN){
            e = vlan_accept(ls, ns);
            ip = ns->raddr; port = ns->rport;
        }
#ifndef __wasm__
        else {
            struct sockaddr_in a; socklen_t al = sizeof a;
            int fd = accept(ls->fd, (struct sockaddr *)&a, &al);
            if (fd < 0) e = ws_errno_to_wsa(errno);
            else {
                ws_fd_setup(fd); ns->fd = fd;
                host_to_guest(&a, &ip, &port);
                ns->raddr = ip; ns->rport = port;
            }
        }
#endif
        if (!e){
            ns->state = ST_CONNECTED; ns->bound = 1; ns->nb = ls->nb;      /* Windows: properties of the listener */
            ns->so_keepalive = ls->so_keepalive; ns->so_oobinline = ls->so_oobinline;
            if (pa){ int e2 = ga_out(pa, pl, ip, port); (void)e2; }
            return shandle(ns);
        }
        ns->used = 0;
        if (e != WSAEWOULDBLOCK || ls->nb) FAIL(e);
        int w = swait(ls, POLLIN, 0);
        ls = sget(h);
        if (w == 2 || !ls) FAIL(WSAEINTR);
    }
}

/* Non-blocking TCP connect completion (host side). */
static void conn_poll(WsSock *s, short rev){
    if (s->state != ST_CONNECTING) return;
    if (g_ws_mode == NET_VLAN){
#ifdef __wasm__
        vlan_conn_update(s);         /* the other browser's answer, if it came (net_lan.c) */
#endif
        if (ws_now_us() < s->conn_ready_us) return;
    }
#ifndef __wasm__
    else {
        if (!(rev & (POLLOUT | POLLERR | POLLHUP))) return;
        int err = 0; socklen_t l = sizeof err;
        getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &l);
        s->conn_result = err ? ws_errno_to_wsa(err) : 0;
        if (err == EHOSTUNREACH || err == EPERM || err == EACCES) deny_note(err, 0);
    }
#else
    (void)rev;
#endif
    if (s->conn_result){ s->state = ST_FAILED; s->pending_err = s->conn_result; }
    else { s->state = ST_CONNECTED; if (s->raddr != g_ws_id.ip && !is_loop(s->raddr)) deny_ok(); }
}

static uint32_t ws_connect(CPU *c){
    if (need_startup()) return 0xFFFFFFFFu;
    WsSock *s = sget(A(c, 0)); if (!s) FAIL(WSAENOTSOCK);
    uint32_t ip; uint16_t port;
    int e = ga_in(A(c, 1), A(c, 2), &ip, &port); if (e) FAIL(e);
    uint32_t h = shandle(s);
    if (s->type != 1){
        /* UDP (and raw): a default destination.  No packet.  SocketHost's
         * fallback route probe (UDP connect toward 24.1.1.1) comes here. */
        if (!ip && !port){ s->has_peer = 0; return 0; }
#ifndef __wasm__
        if (g_ws_mode == NET_LOOP){
            struct sockaddr_in hd[2]; int er = 0;
            if (loop_dest(ip, port, 1, hd, &er) < 0){ blocked("connect(udp)", ip, port, c); FAIL(er); }
        }
#endif
        e = implicit_bind(s); if (e) FAIL(e);
#ifndef __wasm__
        if (g_ws_mode == NET_LAN && s->fd >= 0 && !s->icmp_fake){
            struct sockaddr_in hd; memset(&hd, 0, sizeof hd); hd.sin_len = sizeof hd; hd.sin_family = AF_INET;
            hd.sin_port = htons(port); hd.sin_addr.s_addr = ip;
            if (connect(s->fd, (struct sockaddr *)&hd, sizeof hd) != 0){
                int er = errno; if (er == EHOSTUNREACH || er == EPERM || er == EACCES) deny_note(er, 0);
                FAIL(ws_errno_to_wsa(er)); }
        }
#endif
        s->has_peer = 1; s->raddr = ip; s->rport = port; s->udp_reset = 0;
        return 0;
    }
    if (s->state == ST_CONNECTING) FAIL(WSAEALREADY);
    if (s->state == ST_CONNECTED) FAIL(WSAEISCONN);
    if (s->state == ST_LISTEN) FAIL(WSAEINVAL);
    if (!ip || ip == 0xFFFFFFFFu || !port) FAIL(ip ? WSAEADDRNOTAVAIL : WSAEADDRNOTAVAIL);
    s->raddr = ip; s->rport = port;
    { int de = deny_sim(ip);
      if (de){                       /* refused at once; reported like every other failure (asynchronously) */
          deny_note(de, 1);
          s->bound = 1; s->state = ST_FAILED; s->conn_result = s->pending_err = ws_errno_to_wsa(de);
          if (s->nb) FAIL(WSAEWOULDBLOCK);
          s->state = ST_BOUND; s->pending_err = 0; FAIL(s->conn_result);
      } }
    if (g_ws_mode == NET_VLAN){
        e = vlan_connect(s, ip, port);          /* result known now, reported at conn_ready_us */
        if (e && e != WSAECONNREFUSED && e != WSAETIMEDOUT && e != WSAEHOSTUNREACH){ FAIL(e); }
        s->bound = 1; s->state = ST_CONNECTING; s->conn_result = e;
    }
#ifndef __wasm__
    else {
        struct sockaddr_in hd[2]; int n = 1, er = 0;
        if (g_ws_mode == NET_LOOP){
            n = loop_dest(ip, port, 0, hd, &er);
            if (n < 0){ blocked("connect", ip, port, c); FAIL(er); }
        } else { memset(hd, 0, sizeof hd[0]); hd[0].sin_len = sizeof hd[0]; hd[0].sin_family = AF_INET;
                 hd[0].sin_port = htons(port); hd[0].sin_addr.s_addr = ip; }
        s->state = ST_CONNECTING; s->conn_result = 0; s->bound = 1;
        if (n == 0){ s->conn_result = er; s->state = ST_FAILED; s->pending_err = er; }
        else if (connect(s->fd, (struct sockaddr *)&hd[0], sizeof hd[0]) == 0){
            s->state = ST_CONNECTED; if (ip != g_ws_id.ip && !is_loop(ip)) deny_ok(); }
        else {
            er = errno;
            if (er == EINPROGRESS || er == EINTR) { /* pending */ }
            else if (er == ECONNREFUSED || er == ETIMEDOUT || er == ENETUNREACH || er == EHOSTUNREACH ||
                     er == EPERM || er == EACCES || er == ENETDOWN || er == EHOSTDOWN || er == ECONNRESET){
                /* Reported asynchronously (exceptfds / SO_ERROR), as Windows does
                 * for a non-blocking connect: the game's lobby code hangs on a
                 * synchronous failure (measured with a simulated denial).
                 * EPERM/EACCES: a sandbox, or macOS Local Network privacy. */
                s->conn_result = ws_errno_to_wsa(er); s->state = ST_FAILED; s->pending_err = s->conn_result;
                if (er == EHOSTUNREACH || er == EPERM || er == EACCES) deny_note(er, 1);
            } else { s->state = ST_BOUND; FAIL(ws_errno_to_wsa(er)); }
        }
    }
#endif
    if (s->nb){
        /* Windows: a non-blocking TCP connect never completes inside the call */
        if (s->state == ST_CONNECTED && g_ws_mode != NET_VLAN) { /* reported writable by select */ }
        FAIL(WSAEWOULDBLOCK);
    }
    /* blocking connect: wait for the outcome with the guest lock released */
    for (;;){
        s = sget(h); if (!s) FAIL(WSAEINTR);
        if (s->state == ST_CONNECTING){
            if (g_ws_mode == NET_VLAN){
                uint64_t now = ws_now_us();
                if (now < s->conn_ready_us){ int w = swait(s, 0, s->conn_ready_us); if (w == 2) FAIL(WSAEINTR); }
                conn_poll(s, 0);
            } else {
                int w = swait(s, POLLOUT, 0); if (w == 2) FAIL(WSAEINTR);
                s = sget(h); if (!s) FAIL(WSAEINTR);
                struct pollfd p = { s->fd, POLLOUT, 0 }; poll(&p, 1, 0); conn_poll(s, p.revents);
            }
            continue;
        }
        if (s->state == ST_FAILED){ int r = s->conn_result; s->state = ST_BOUND; s->pending_err = 0; FAIL(r); }
        return 0;
    }
}

/* ---- data ---- */
static int send_one(WsSock *s, const uint8_t *buf, int len, int wflags, uint32_t ip, uint16_t port, int has_addr, int *sent, CPU *c){
    *sent = 0;
    if (s->type == 1){
        if (s->state != ST_CONNECTED) return WSAENOTCONN;
        if (s->shut_wr) return WSAESHUTDOWN;
        ssize_t n = send(s->fd, buf, (size_t)len, wflags | MSG_NOSIGNAL);
        if (n < 0){
            int e = errno;
            if (e == EPIPE && g_ws_mode == NET_VLAN && !vlan_conn_was_reset(s)) return WSAECONNRESET;
            return ws_errno_to_wsa(e);
        }
        if (n > 0 && g_deny_t0 && s->raddr != g_ws_id.ip && !is_loop(s->raddr)) deny_ok();
        *sent = (int)n; return 0;
    }
    /* datagram / raw */
    if (!has_addr){
        if (!s->has_peer) return WSAENOTCONN;
        ip = s->raddr; port = s->rport;
    }
    if (s->shut_wr) return WSAESHUTDOWN;
    if (s->type == 2 && len > 65507) return WSAEMSGSIZE;
    if (s->type == 2 && is_bcast(ip) && !s->so_broadcast) return WSAEACCES;
    int e = implicit_bind(s); if (e) return e;
    int beyond = ip != g_ws_id.ip && !is_loop(ip);       /* to another machine (or a broadcast) */
    { int de = deny_sim(ip); if (de){ if (is_bcast(ip)) deny_keep(s, buf, len, ip, port);
                                       deny_note(de, is_bcast(ip)); return ws_errno_to_wsa(de); } }
    if (g_ws_mode == NET_VLAN){
        e = vlan_sendto(s, buf, len, ip, port, sent);
        if (!e && beyond) deny_ok();
        return e;
    }
#ifdef __wasm__
    (void)c; (void)wflags; return WSAENETDOWN;          /* no BSD sockets in a browser */
#else
    if (s->icmp_fake){ *sent = len; return 0; }
    struct sockaddr_in hd[2]; int n = 1;
    if (g_ws_mode == NET_LOOP && s->type == 3 && len >= 8 && buf[0] == 8){
        /* echo request: the kernel answers it from 127.0.0.1; remember whom
         * the guest pinged, so the reply comes from there (recv_one) */
        int k = s->echo_n++ % 8;
        s->echo_key[k] = (uint32_t)buf[4] << 24 | (uint32_t)buf[5] << 16 | (uint32_t)buf[6] << 8 | buf[7];
        s->echo_dst[k] = ip;
    }
    if (g_ws_mode == NET_LOOP){
        int er = 0; n = loop_dest(ip, port, 1, hd, &er);
        if (n < 0){ blocked(s->type == 3 ? "ping" : "sendto", ip, port, c); return er; }
        if (n == 0){ *sent = len; return 0; }            /* no listener on the peer: lost, like a real LAN */
    } else {
        memset(&hd[0], 0, sizeof hd[0]); hd[0].sin_len = sizeof hd[0]; hd[0].sin_family = AF_INET;
        hd[0].sin_port = htons(port); hd[0].sin_addr.s_addr = ip;
    }
    /* lan: 255.255.255.255 leaves through the LAN interface even when a VPN
     * holds the default route */
    int bound_if = g_ws_mode == NET_LAN && ip == 0xFFFFFFFFu && g_ws_id.ifindex;
    if (bound_if) setsockopt(s->fd, IPPROTO_IP, IP_BOUND_IF, &g_ws_id.ifindex, sizeof g_ws_id.ifindex);
    ssize_t r = -1; int er = 0;
    for (int i = 0; i < n; i++){
        ssize_t k = (s->has_peer && g_ws_mode == NET_LAN) ? send(s->fd, buf, (size_t)len, wflags)
                                                          : sendto(s->fd, buf, (size_t)len, wflags, (struct sockaddr *)&hd[i], sizeof hd[i]);
        if (k >= 0){ if (r < 0) r = k; }
        else if (i == 0) er = errno;
    }
    if (bound_if){ int z = 0; setsockopt(s->fd, IPPROTO_IP, IP_BOUND_IF, &z, sizeof z); }
    if (r < 0){
        if (er == EHOSTUNREACH || er == EPERM || er == EACCES){
            if (is_bcast(ip)) deny_keep(s, buf, len, ip, port);
            deny_note(er, is_bcast(ip)); }
        if (er == ECONNREFUSED && s->type == 2) return WSAECONNRESET;
        return ws_errno_to_wsa(er);
    }
    if (beyond) deny_ok();
    *sent = (int)r; return 0;
#endif
}

static uint32_t do_send(CPU *c, int has_addr){
    if (need_startup()) return 0xFFFFFFFFu;
    uint32_t h = A(c, 0), buf = A(c, 1); int len = (int)A(c, 2); uint32_t fl = A(c, 3);
    WsSock *s = sget(h); if (!s) FAIL(WSAENOTSOCK);
    if (len < 0 || (len && !gok(buf, (uint32_t)len))) FAIL(WSAEFAULT);
    uint32_t ip = 0; uint16_t port = 0;
    if (has_addr && s->type != 1){
        int e = ga_in(A(c, 4), A(c, 5), &ip, &port); if (e) FAIL(e);
    }
    if (fl & ~0x5u){ static int said; if (!said++) ws_log("send flags 0x%x ignored", fl); }
    int wf = (fl & 1u ? MSG_OOB : 0) | (fl & 4u ? MSG_DONTROUTE : 0);
    int total = 0;
    uint64_t dl = deadline_ms(s->sndtimeo);
    /* what the game says to the online host's lobby: a search with the Filter the player set is marked (olfilter.c) */
    uint32_t lob = s->type == 1 && g_online && s->raddr == mkip(10, 77, 0, 1) && s->rport == ONLINE_LOBBY
                   ? (s->conn_id ? s->conn_id : h) : 0;
    for (;;){
        int sent = 0;
        if (lob) olfilter_lobby_mark(lob, g_mem + buf + total, len - total);
        int e = send_one(s, g_mem + buf + total, len - total, wf, ip, port, has_addr && s->type != 1, &sent, c);
        if (!e){
            if (lob) olfilter_lobby_sent(lob, g_mem + buf + total, sent);
            total += sent; st_sent++; st_bytes_out += (uint64_t)sent;
            if (s->type != 1 || total >= len || s->nb) return (uint32_t)total;
            continue;                            /* blocking stream send: everything */
        }
        if (e != WSAEWOULDBLOCK || s->nb){ if (total) return (uint32_t)total; FAIL(e); }
        int w = swait(s, POLLOUT, dl);
        s = sget(h);
        if (w == 2 || !s) FAIL(WSAEINTR);
        if (w == 1){ if (total) return (uint32_t)total; FAIL(WSAETIMEDOUT); }
    }
}

/* Windows raw ICMP sockets deliver the IP header: normalise the one macOS
 * puts there (ip_len in host order without the header) to wire format. */
static void fix_ip_header(uint8_t *b, int n){
    if (n < 20 || (b[0] >> 4) != 4) return;
    b[2] = (uint8_t)(n >> 8); b[3] = (uint8_t)n;
    b[10] = b[11] = 0;
    uint32_t sum = 0; for (int i = 0; i < 20; i += 2) sum += (uint32_t)(b[i] << 8 | b[i + 1]);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    sum = ~sum & 0xFFFF; b[10] = (uint8_t)(sum >> 8); b[11] = (uint8_t)sum;
}

static int recv_one(WsSock *s, uint8_t *buf, int len, int peek, uint32_t *ip, uint16_t *port, int *got){
    *got = 0;
    if (s->type == 1){
        if (s->state != ST_CONNECTED) return WSAENOTCONN;
        if (s->shut_rd) return WSAESHUTDOWN;
        ssize_t n = recv(s->fd, buf, (size_t)len, peek ? MSG_PEEK : 0);
        if (n < 0) return ws_errno_to_wsa(errno);
        if (n == 0){ static int dbg = -1; if (dbg < 0){ const char *e = getenv("NFSU2_VLAN_DEBUG"); dbg = e && *e && *e != '0'; }
                     if (dbg) ws_log("recv: EOF on fd %d (conn #%u)", s->fd, s->conn_id); }
        if (n == 0 && len > 0 && g_ws_mode == NET_VLAN && vlan_conn_was_reset(s)) return WSAECONNRESET;
        *got = (int)n; *ip = s->raddr; *port = s->rport;
        return 0;
    }
    if (!s->bound) return WSAEINVAL;
    if (s->shut_rd) return WSAESHUTDOWN;
    if (s->udp_reset && s->has_peer){ s->udp_reset = 0; return WSAECONNRESET; }
    if (g_ws_mode == NET_VLAN) return vlan_recvfrom(s, buf, len, peek, ip, port, got);
#ifdef __wasm__
    return WSAENETDOWN;                                   /* no BSD sockets in a browser */
#else
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    struct iovec io = { buf, (size_t)len };
    struct msghdr m; memset(&m, 0, sizeof m);
    m.msg_name = &a; m.msg_namelen = sizeof a; m.msg_iov = &io; m.msg_iovlen = 1;
    ssize_t n = recvmsg(s->fd, &m, peek ? MSG_PEEK : 0);
    if (n < 0){
        int e = errno;
        if (e == ECONNREFUSED) return WSAECONNRESET;    /* connected UDP: ICMP port unreachable */
        return ws_errno_to_wsa(e);
    }
    host_to_guest(&a, ip, port);
    if (s->type == 3 && g_ws_mode == NET_LOOP && n >= 28 && len >= 28 && (buf[0] >> 4) == 4 && buf[20] == 0){
        uint32_t key = (uint32_t)buf[24] << 24 | (uint32_t)buf[25] << 16 | (uint32_t)buf[26] << 8 | buf[27];
        for (int k = 0; k < 8 && k < s->echo_n; k++) if (s->echo_key[k] == key){
            *ip = s->echo_dst[k]; *port = 0;
            memcpy(buf + 12, &s->echo_dst[k], 4); memcpy(buf + 16, &g_ws_id.ip, 4);
            break;
        }
    }
    if (s->type == 3) fix_ip_header(buf, (int)n);
    if (m.msg_flags & MSG_TRUNC){ *got = len; return WSAEMSGSIZE; }   /* head copied, rest discarded */
    *got = (int)n;
    return 0;
#endif
}

static uint32_t do_recv(CPU *c, int has_from){
    if (need_startup()) return 0xFFFFFFFFu;
    uint32_t h = A(c, 0), buf = A(c, 1); int len = (int)A(c, 2); uint32_t fl = A(c, 3);
    uint32_t pf = has_from ? A(c, 4) : 0, pfl = has_from ? A(c, 5) : 0;
    WsSock *s = sget(h); if (!s) FAIL(WSAENOTSOCK);
    if (len < 0 || (len && !gok(buf, (uint32_t)len))) FAIL(WSAEFAULT);
    if (pf && (!pfl || !gok(pfl, 4) || (int32_t)MR32(pfl) < 16 || !gok(pf, 16))) FAIL(WSAEFAULT);
    if (fl & ~0x3u){ static int said; if (!said++) ws_log("recv flags 0x%x ignored", fl); }
    if ((fl & 1u) && s->type != 1) FAIL(WSAEOPNOTSUPP);
    uint64_t dl = deadline_ms(s->rcvtimeo);
    for (;;){
        uint32_t ip = 0; uint16_t port = 0; int got = 0;
        int e = recv_one(s, g_mem + buf, len, (fl & 2u) != 0, &ip, &port, &got);
        if (!e || e == WSAEMSGSIZE){
            st_recv++; st_bytes_in += (uint64_t)got;
            if (g_deny_strong && (got > 0 || s->type != 1)) deny_heard(ip);
            if (pf && s->type != 1) ga_out(pf, pfl, ip, port);
            /* what the online host's lobby says, as the game reads it (not a peek) */
            if (!e && got > 0 && !(fl & 2u) && s->type == 1 && g_online && s->raddr == mkip(10, 77, 0, 1) && s->rport == ONLINE_LOBBY)
                online_lobby_read(s->conn_id ? s->conn_id : h, g_mem + buf, got);
            if (e) FAIL(e);
            return (uint32_t)got;
        }
        if (e != WSAEWOULDBLOCK || s->nb) FAIL(e);
        int w = swait(s, POLLIN, dl);
        s = sget(h);
        if (w == 2 || !s) FAIL(WSAEINTR);
        if (w == 1) FAIL(WSAETIMEDOUT);
    }
}

static uint32_t ws_shutdown(CPU *c){
    if (need_startup()) return 0xFFFFFFFFu;
    WsSock *s = sget(A(c, 0)); if (!s) FAIL(WSAENOTSOCK);
    int how = (int)A(c, 1);
    if (how < 0 || how > 2) FAIL(WSAEINVAL);
    if (s->type == 1){
        if (s->state != ST_CONNECTED) FAIL(WSAENOTCONN);
        if (shutdown(s->fd, how == 0 ? SHUT_RD : how == 1 ? SHUT_WR : SHUT_RDWR) != 0 && errno != ENOTCONN)
            FAIL(ws_errno_to_wsa(errno));
    }
    /* datagram sockets: Windows only stops send / receive (no error) */
    if (how != 1) s->shut_rd = 1;
    if (how != 0) s->shut_wr = 1;
    return 0;
}

static uint32_t ws_getname(CPU *c, int peer){
    if (need_startup()) return 0xFFFFFFFFu;
    WsSock *s = sget(A(c, 0)); if (!s) FAIL(WSAENOTSOCK);
    uint32_t p = A(c, 1), pl = A(c, 2);
    if (!p || !pl || !gok(pl, 4) || (int32_t)MR32(pl) < 16 || !gok(p, 16)) FAIL(WSAEFAULT);
    uint32_t ip = 0; uint16_t port = 0;
    if (peer){
        if (s->type == 1){
            if (s->state == ST_CONNECTING){ struct pollfd q = { s->fd, POLLOUT, 0 }; if (g_ws_mode != NET_VLAN && s->fd >= 0) poll(&q, 1, 0); conn_poll(s, q.revents); }
            if (s->state != ST_CONNECTED) FAIL(WSAENOTCONN);
            if (g_ws_mode == NET_VLAN){
                if (vlan_conn_was_reset(s)) FAIL(WSAENOTCONN);        /* reset: no longer connected */
            }
#ifndef __wasm__
            else {
                /* after a reset macOS says EINVAL; Windows says WSAENOTCONN,
                 * which is how DirtySock notices a dropped connection */
                struct sockaddr_in a; socklen_t al = sizeof a;
                if (getpeername(s->fd, (struct sockaddr *)&a, &al) != 0){
                    int e = errno; FAIL(e == EINVAL || e == ENOTCONN ? WSAENOTCONN : ws_errno_to_wsa(e)); }
            }
#endif
            ip = s->raddr; port = s->rport;
        } else {
            if (!s->has_peer) FAIL(WSAENOTCONN);
            ip = s->raddr; port = s->rport;
        }
    } else {
        if (!s->bound) FAIL(WSAEINVAL);
        if (g_ws_mode == NET_VLAN){ ip = s->laddr; port = s->lport; }
        else bsd_local(s, &ip, &port);
    }
    ga_out(p, pl, ip, port);
    return 0;
}

/* ---- options ---- */
static int optint(uint32_t p, uint32_t l, int *v){
    if (!p || (int32_t)l < 1 || !gok(p, l < 4 ? l : 4)) return WSAEFAULT;
    *v = l >= 4 ? (int)MR32(p) : (int)MR8(p);
    return 0;
}
static uint32_t ws_setsockopt(CPU *c){
    if (need_startup()) return 0xFFFFFFFFu;
    WsSock *s = sget(A(c, 0)); if (!s) FAIL(WSAENOTSOCK);
    uint32_t lvl = A(c, 1), opt = A(c, 2), p = A(c, 3), l = A(c, 4);
    int v = 0, e = 0, hfd = (g_ws_mode != NET_VLAN && s->fd >= 0 && !s->icmp_fake) ? s->fd : -1;
    if (lvl == 0xFFFF){
        switch (opt){
        case 0x0004: /* SO_REUSEADDR */
            if ((e = optint(p, l, &v))) FAIL(e);
            s->so_reuse = v != 0;
            if (hfd >= 0){ int one = v != 0; setsockopt(hfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
                           if (s->type == 2) setsockopt(hfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one); }
            return 0;
        case 0xFFFFFFFBu: /* SO_EXCLUSIVEADDRUSE */
            if ((e = optint(p, l, &v))) FAIL(e);
            return 0;
        case 0x0020: /* SO_BROADCAST */
            if ((e = optint(p, l, &v))) FAIL(e);
            if (s->type == 1) FAIL(WSAENOPROTOOPT);
            s->so_broadcast = v != 0;
            if (hfd >= 0){ int b = v != 0; setsockopt(hfd, SOL_SOCKET, SO_BROADCAST, &b, sizeof b); }
            return 0;
        case 0x0008: /* SO_KEEPALIVE */
            if ((e = optint(p, l, &v))) FAIL(e);
            s->so_keepalive = v != 0;
            if (hfd >= 0 && s->type == 1){ int b = v != 0; setsockopt(hfd, SOL_SOCKET, SO_KEEPALIVE, &b, sizeof b); }
            return 0;
        case 0x0010: /* SO_DONTROUTE */
            if ((e = optint(p, l, &v))) FAIL(e);
            s->so_dontroute = v != 0; return 0;
        case 0x0100: /* SO_OOBINLINE */
            if ((e = optint(p, l, &v))) FAIL(e);
            s->so_oobinline = v != 0;
            if (hfd >= 0 && s->type == 1){ int b = v != 0; setsockopt(hfd, SOL_SOCKET, SO_OOBINLINE, &b, sizeof b); }
            return 0;
        case 0x0080: /* SO_LINGER: struct linger { u_short l_onoff, l_linger } */
            if (!p || (int32_t)l < 4 || !gok(p, 4)) FAIL(WSAEFAULT);
            if (s->type != 1) FAIL(WSAENOPROTOOPT);
            s->linger_on = MR16(p) != 0; s->linger_s = MR16(p + 2);
            if (hfd >= 0){ struct linger lg = { s->linger_on, s->linger_s }; setsockopt(hfd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg); }
            return 0;
        case 0xFFFFFF7Fu: /* SO_DONTLINGER */
            if ((e = optint(p, l, &v))) FAIL(e);
            if (s->type != 1) FAIL(WSAENOPROTOOPT);
            if (v){ s->linger_on = 0; if (hfd >= 0){ struct linger lg = { 0, 0 }; setsockopt(hfd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg); } }
            return 0;
        case 0x1001: case 0x1002: /* SO_SNDBUF / SO_RCVBUF */
            if ((e = optint(p, l, &v))) FAIL(e);
            if (opt == 0x1001) s->sndbuf = v; else s->rcvbuf = v;
            if (hfd >= 0 && v > 0){ setsockopt(hfd, SOL_SOCKET, opt == 0x1001 ? SO_SNDBUF : SO_RCVBUF, &v, sizeof v); }
            return 0;
        case 0x1005: case 0x1006: /* SO_SNDTIMEO / SO_RCVTIMEO: DWORD ms, emulated in the wait loop */
            if ((e = optint(p, l, &v))) FAIL(e);
            if (opt == 0x1005) s->sndtimeo = (uint32_t)v; else s->rcvtimeo = (uint32_t)v;
            return 0;
        default: break;
        }
    } else if (lvl == 0 /* IPPROTO_IP */){
        switch (opt){
        case 4: /* IP_TTL */
            if ((e = optint(p, l, &v))) FAIL(e);
            if (v < 0 || v > 255) FAIL(WSAEINVAL);
            s->ip_ttl = v;
            if (hfd >= 0){ setsockopt(hfd, IPPROTO_IP, IP_TTL, &v, sizeof v); }
            return 0;
        case 3: /* IP_TOS */
            if ((e = optint(p, l, &v))) FAIL(e);
            if (hfd >= 0){ setsockopt(hfd, IPPROTO_IP, IP_TOS, &v, sizeof v); }
            return 0;
        default: break;
        }
    } else if (lvl == 6 /* IPPROTO_TCP */ && opt == 1 /* TCP_NODELAY */){
        if ((e = optint(p, l, &v))) FAIL(e);
        if (s->type != 1) FAIL(WSAENOPROTOOPT);
        if (hfd >= 0){ int b = v != 0; setsockopt(hfd, IPPROTO_TCP, TCP_NODELAY, &b, sizeof b); }
        return 0;
    }
    { static uint32_t seen[16]; static int ns; int k;
      for (k = 0; k < ns && seen[k] != (lvl << 16 ^ opt); k++) {}
      if (k == ns && ns < 16){ seen[ns++] = lvl << 16 ^ opt; ws_log("setsockopt(level 0x%x, option 0x%x) not supported -> WSAENOPROTOOPT", lvl, opt); } }
    FAIL(lvl == 0xFFFF || lvl == 0 || lvl == 6 ? WSAENOPROTOOPT : WSAEINVAL);
}

static uint32_t ws_getsockopt(CPU *c){
    if (need_startup()) return 0xFFFFFFFFu;
    WsSock *s = sget(A(c, 0)); if (!s) FAIL(WSAENOTSOCK);
    uint32_t lvl = A(c, 1), opt = A(c, 2), p = A(c, 3), pl = A(c, 4);
    if (!p || !pl || !gok(pl, 4) || (int32_t)MR32(pl) < 1) FAIL(WSAEFAULT);
    int32_t cap = (int32_t)MR32(pl);
    uint32_t v = 0; int bytes = 4;
    if (lvl == 0xFFFF){
        switch (opt){
        case 0x0002: v = s->state == ST_LISTEN; break;              /* SO_ACCEPTCONN */
        case 0x0004: v = s->so_reuse; break;
        case 0x0008: v = s->so_keepalive; break;
        case 0x0010: v = s->so_dontroute; break;
        case 0x0020: v = s->so_broadcast; break;
        case 0x0100: v = s->so_oobinline; break;
        case 0x0080:                                                  /* SO_LINGER (4 bytes) */
            if (cap < 4 || !gok(p, 4)) FAIL(WSAEFAULT);
            MR16(p) = (uint16_t)s->linger_on; MR16(p + 2) = (uint16_t)s->linger_s; MR32(pl) = 4; return 0;
        case 0xFFFFFF7Fu: v = !s->linger_on; break;
        case 0x1001: v = (uint32_t)s->sndbuf; break;
        case 0x1002: v = (uint32_t)s->rcvbuf; break;
        case 0x1005: v = s->sndtimeo; break;
        case 0x1006: v = s->rcvtimeo; break;
        case 0x1007:                                                  /* SO_ERROR, then cleared */
            if (s->type == 1 && s->state == ST_CONNECTING){
                struct pollfd q = { s->fd, POLLOUT, 0 };
                if (g_ws_mode != NET_VLAN && s->fd >= 0) poll(&q, 1, 0);
                conn_poll(s, q.revents);
            }
            v = (uint32_t)s->pending_err; s->pending_err = 0;
            if (!v && g_ws_mode != NET_VLAN && s->fd >= 0 && !s->icmp_fake){
                int er = 0; socklen_t l = sizeof er;
                if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &er, &l) == 0 && er) v = (uint32_t)ws_errno_to_wsa(er);
            }
            break;
        case 0x1008: v = (uint32_t)s->type; break;                   /* SO_TYPE */
        default: goto unk;
        }
    } else if (lvl == 0 && opt == 4) v = (uint32_t)s->ip_ttl;
    else if (lvl == 6 && opt == 1){
        int b = 0; socklen_t l = sizeof b;
        if (g_ws_mode != NET_VLAN && s->fd >= 0 && s->type == 1) getsockopt(s->fd, IPPROTO_TCP, TCP_NODELAY, &b, &l);
        v = b != 0;
    } else goto unk;
    /* BOOL and int options: Windows accepts a 1-byte buffer for BOOLs */
    if (cap < 4) bytes = cap >= 1 ? 1 : 0;
    if (!bytes || !gok(p, (uint32_t)bytes)) FAIL(WSAEFAULT);
    if (bytes == 4) MR32(p) = v; else MR8(p) = (uint8_t)v;
    MR32(pl) = (uint32_t)bytes;
    return 0;
unk:
    { static int said; if (said++ < 8) ws_log("getsockopt(level 0x%x, option 0x%x) not supported -> WSAENOPROTOOPT", lvl, opt); }
    FAIL(WSAENOPROTOOPT);
}

static int fionread(WsSock *s, uint32_t *v){
    *v = 0;
    if (s->fd < 0) return 0;
#ifdef __wasm__
    vlan_fionread(s, v); return 0;                        /* the virtual socket's queue (net_lan.c) */
#endif
    if (s->type == 1){ int n = 0; if (ioctl(s->fd, FIONREAD, &n) == 0) *v = (uint32_t)n; return 0; }
    /* datagrams: the size of the next one (macOS FIONREAD counts headers) */
    int n = 0; socklen_t l = sizeof n;
    if (getsockopt(s->fd, SOL_SOCKET, SO_NREAD, &n, &l) == 0){
        if (g_ws_mode == NET_VLAN && n >= 8) n -= 8;
        if (s->type == 3 && n > 0 && g_ws_mode == NET_VLAN) n += 20;
        *v = (uint32_t)n;
    }
    return 0;
}
static uint32_t ws_ioctlsocket(CPU *c){
    if (need_startup()) return 0xFFFFFFFFu;
    WsSock *s = sget(A(c, 0)); if (!s) FAIL(WSAENOTSOCK);
    uint32_t cmd = A(c, 1), p = A(c, 2);
    if (!p || !gok(p, 4)) FAIL(WSAEFAULT);
    if (cmd == 0x8004667Eu){ s->nb = MR32(p) != 0; return 0; }          /* FIONBIO */
    if (cmd == 0x4004667Fu){ uint32_t v; fionread(s, &v); MR32(p) = v; return 0; }  /* FIONREAD */
    if (cmd == 0x40047307u){ MR32(p) = 1; return 0; }                    /* SIOCATMARK: no OOB data */
    { static int said; if (said++ < 8) ws_log("ioctlsocket(0x%08x) not supported -> WSAEINVAL", cmd); }
    FAIL(WSAEINVAL);
}

static uint32_t ws_wsaioctl(CPU *c){
    if (need_startup()) return 0xFFFFFFFFu;
    WsSock *s = sget(A(c, 0)); if (!s) FAIL(WSAENOTSOCK);
    uint32_t code = A(c, 1), in = A(c, 2), inl = A(c, 3), out = A(c, 4), outl = A(c, 5), pret = A(c, 6), ov = A(c, 7);
    if (ov){ static int said; if (!said++) ws_log("WSAIoctl with OVERLAPPED: completed synchronously"); }
    if (code == 0xC8000014u){                                   /* SIO_ROUTING_INTERFACE_QUERY */
        uint32_t ip; uint16_t port;
        int e = ga_in(in, inl, &ip, &port); if (e) FAIL(e);
        if (!out || outl < 16u || !gok(out, 16)) { if (pret && gok(pret, 4)) MR32(pret) = 16; FAIL(WSAEFAULT); }
        uint32_t loc = 0;
        e = route_query(ip, &loc);
        if (e) FAIL(e);
        MR16(out) = 2; MR16(out + 2) = 0; MR32(out + 4) = loc; MR32(out + 8) = 0; MR32(out + 12) = 0;
        if (pret && gok(pret, 4)) MR32(pret) = 16;
        return 0;
    }
    if (code == 0x8004667Eu || code == 0x4004667Fu){              /* FIONBIO / FIONREAD */
        if (code == 0x8004667Eu){ if (!in || inl < 4 || !gok(in, 4)) FAIL(WSAEFAULT); s->nb = MR32(in) != 0;
                                  if (pret && gok(pret, 4)) MR32(pret) = 0; return 0; }
        if (!out || outl < 4 || !gok(out, 4)) FAIL(WSAEFAULT);
        uint32_t v; fionread(s, &v); MR32(out) = v; if (pret && gok(pret, 4)) MR32(pret) = 4; return 0;
    }
    if (code == 0x9800000Cu){ if (pret && gok(pret, 4)) MR32(pret) = 0; return 0; }   /* SIO_UDP_CONNRESET */
    { static uint32_t seen[8]; static int ns; int k;
      for (k = 0; k < ns && seen[k] != code; k++) {}
      if (k == ns && ns < 8){ seen[ns++] = code; ws_log("WSAIoctl(0x%08x) not supported -> WSAEOPNOTSUPP", code); } }
    FAIL(WSAEOPNOTSUPP);
}

/* ---- select ---- */
#define SEL_MAX 4096                    /* server.dll builds fd_sets of up to 0x1000 entries */
typedef struct { WsSock *s; int fd; short ev; } SelEnt;
static uint32_t ws_select(CPU *c){
    if (need_startup()) return 0xFFFFFFFFu;
    uint32_t set[3] = { A(c, 1), A(c, 2), A(c, 3) }, tv = A(c, 4);
    uint32_t cnt[3] = { 0, 0, 0 }, total = 0;
    for (int k = 0; k < 3; k++){
        if (!set[k]) continue;
        if (!gok(set[k], 4)) FAIL(WSAEFAULT);
        cnt[k] = MR32(set[k]);
        if (cnt[k] > SEL_MAX) FAIL(WSAEINVAL);
        if (cnt[k] && !gok(set[k] + 4, 4 * cnt[k])) FAIL(WSAEFAULT);
        total += cnt[k];
    }
    if (!total) FAIL(WSAEINVAL);
    /* Every set is read before any is written back: the same fd_set may be
     * passed as write and except set (server.dll's connect check). */
    uint32_t *hs = malloc(sizeof *hs * total);
    uint8_t *rdy = calloc(total, 1);
    int *map = malloc(sizeof *map * total);
    SelEnt *pe = malloc(sizeof *pe * total);
    struct pollfd *pf = malloc(sizeof *pf * total);
    uint32_t r = 0xFFFFFFFFu; int err = 0;
    { uint32_t i = 0;
      for (int k = 0; k < 3; k++) for (uint32_t j = 0; j < cnt[k]; j++){
          hs[i] = MR32(set[k] + 4 + 4 * j);
          if (!sget(hs[i])){ err = WSAENOTSOCK; goto out; }
          i++; } }
    uint64_t dl = 0; int forever = 1;
    if (tv){
        if (!gok(tv, 8)){ err = WSAEFAULT; goto out; }
        int32_t sec = (int32_t)MR32(tv), usec = (int32_t)MR32(tv + 4);
        if (sec < 0) sec = 0;
        if (usec < 0) usec = 0;
        dl = ws_now_us() + (uint64_t)sec * 1000000ull + (uint64_t)usec; forever = 0;
    }
    st_select++;
    for (;;){
        /* one pollfd per distinct socket */
        int np = 0;
        { uint32_t i = 0;
          for (int k = 0; k < 3; k++) for (uint32_t j = 0; j < cnt[k]; j++, i++){
              WsSock *s = sget(hs[i]);
              if (!s){ err = WSAENOTSOCK; goto out; }
              int q; for (q = 0; q < np && pe[q].s != s; q++) {}
              if (q == np){ pe[np].s = s; pe[np].fd = s->fd; pe[np].ev = 0; np++; }
              pe[q].ev |= k == 0 ? POLLIN : k == 1 ? POLLOUT : 0;
              if (s->type == 1 && s->state == ST_CONNECTING) pe[q].ev |= POLLOUT;
              map[i] = q;
          } }
        for (int q = 0; q < np; q++){ pf[q].fd = pe[q].fd; pf[q].events = pe[q].fd >= 0 ? pe[q].ev : 0; pf[q].revents = 0; }
        poll(pf, (nfds_t)np, 0);
        int ready = 0; uint64_t next_conn = 0;
        { uint32_t i = 0;
          for (int k = 0; k < 3; k++) for (uint32_t j = 0; j < cnt[k]; j++, i++){
              int q = map[i]; WsSock *s = pe[q].s;
              short rev = pf[q].fd >= 0 ? pf[q].revents : 0;
              int rd = 0, wr = 0, ex = 0;
              if (s->type == 1){
                  if (s->state == ST_CONNECTING){
                      conn_poll(s, rev);
                      if (s->state == ST_CONNECTING && g_ws_mode == NET_VLAN && (!next_conn || s->conn_ready_us < next_conn))
                          next_conn = s->conn_ready_us;
                      if (s->state == ST_CONNECTED){ wr = 1; rev &= (short)~(POLLIN | POLLHUP); }
                  }
                  if (s->state == ST_LISTEN) rd = (rev & POLLIN) != 0;
                  else if (s->state == ST_CONNECTED){
                      rd = (rev & (POLLIN | POLLHUP | POLLERR)) != 0;
                      /* a reset connection is writable on Windows (send fails at
                       * once instead of blocking); BSD only says POLLHUP/POLLERR.
                       * DirtySock notices a drop by "writable, then getpeername
                       * fails with WSAENOTCONN". */
                      wr = wr || (rev & (POLLOUT | POLLHUP | POLLERR)) != 0;
                  } else if (s->state == ST_FAILED) ex = 1;   /* failed connect: exceptfds only */
              } else {
                  rd = (rev & (POLLIN | POLLERR)) != 0 || (s->udp_reset && s->has_peer);
                  wr = s->icmp_fake || pf[q].fd < 0 || (rev & POLLOUT) != 0;
              }
              rdy[i] = (uint8_t)(k == 0 ? rd : k == 1 ? wr : ex);
              ready += rdy[i];
          } }
        uint64_t now = ws_now_us();
        if (ready || (!forever && now >= dl)){
            uint32_t i = 0;
            for (int k = 0; k < 3; k++){
                if (!set[k]) continue;
                uint32_t n = 0;
                for (uint32_t j = 0; j < cnt[k]; j++, i++) if (rdy[i]) MR32(set[k] + 4 + 4 * n++) = hs[i];
                MR32(set[k]) = n;
            }
            r = (uint32_t)ready; goto out;
        }
        /* wait with the guest lock released, in slices of at most 50 ms */
        uint64_t until = forever ? now + 50000ull : dl;
        if (until > now + 50000ull) until = now + 50000ull;
        if (next_conn && next_conn < until) until = next_conn;
        int ms = (int)((until - now + 999) / 1000);
        if (ms < 1) ms = 1;
        for (int q = 0; q < np; q++){
            pe[q].s->waiters++;
            /* a vlan connect completes on a timer; its fd is writable at once */
            if (pe[q].s->type == 1 && pe[q].s->state == ST_CONNECTING && g_ws_mode == NET_VLAN) pf[q].events = 0;
            pf[q].revents = 0;
        }
        guest_wait_begin();
        poll(pf, (nfds_t)np, ms);
        guest_wait_end();
        int closed = 0;
        for (int q = 0; q < np; q++){
            pe[q].s->waiters--;
            if (atomic_load(&pe[q].s->closing)){ closed = 1; if (!pe[q].s->waiters) sfinal(pe[q].s); }
        }
        if (closed){ err = WSAEINTR; goto out; }
    }
out:
    free(hs); free(rdy); free(map); free(pe); free(pf);
    if (err) FAIL(err);
    return r;
}

/* ---- names ---- */
#define GBUF_BASE MM_WSBUF_BASE  /* guest memory of this HLE: per-thread hostent / inet_ntoa (memmap.h) */
#define GBUF_SLOT MM_WSBUF_SLOT
#define GBUF_N    MM_WSBUF_N
static uint32_t g_gbuf_teb[GBUF_N];
static uint32_t gbuf(CPU *c){
    uint32_t teb = c ? c->fs_base : 0;
    static int committed;
    if (!committed){ mem_commit(GBUF_BASE, GBUF_SLOT * GBUF_N); committed = 1; }
    int i, fr = -1;
    for (i = 0; i < GBUF_N; i++){ if (g_gbuf_teb[i] == teb) break; if (!g_gbuf_teb[i] && fr < 0) fr = i; }
    if (i == GBUF_N){ i = fr >= 0 ? fr : (int)((teb >> 13) % GBUF_N); g_gbuf_teb[i] = teb; }
    return GBUF_BASE + GBUF_SLOT * (uint32_t)i;
}
/* hostent { char *h_name; char **h_aliases; short h_addrtype; short h_length; char **h_addr_list } */
static uint32_t make_hostent(CPU *c, const char *name, const uint32_t *ips, int n){
    uint32_t b = gbuf(c);
    if (n > 8) n = 8;
    memset(g_mem + b, 0, 344);
    uint32_t aliases = b + 16, list = b + 20, addrs = b + 56, nm = b + 88;
    snprintf((char *)g_mem + nm, 256, "%s", name);
    MR32(b) = nm; MR32(b + 4) = aliases; MR16(b + 8) = 2; MR16(b + 10) = 4; MR32(b + 12) = list;
    MR32(aliases) = 0;
    for (int i = 0; i < n; i++){ MR32(addrs + 4 * i) = ips[i]; MR32(list + 4 * i) = addrs + 4 * i; }
    MR32(list + 4 * n) = 0;
    return b;
}
static int own_name(const char *n){
    if (!*n) return 1;
    size_t l = strlen(g_ws_id.name);
    if (!strncasecmp(n, g_ws_id.name, l) && (!n[l] || !strcasecmp(n + l, ".local") || !strcasecmp(n + l, ".lan"))) return 1;
    char hn[256] = ""; gethostname(hn, sizeof hn);
    if (g_ws_mode == NET_LAN && *hn && !strcasecmp(n, hn)) return 1;
    return 0;
}
static int own_addrs(uint32_t *ips){
    int n = 0;
    if (g_ws_id.ip) ips[n++] = g_ws_id.ip;
    if (g_ws_mode == NET_LAN) for (int i = 0; i < g_nifs && n < 8; i++) if (g_ifs[i].ip != g_ws_id.ip) ips[n++] = g_ifs[i].ip;
    if (!n) ips[n++] = mkip(127, 0, 0, 1);
    return n;
}
static uint32_t ws_gethostbyname(CPU *c){
    if (need_startup()) return 0;
    uint32_t p = A(c, 0);
    char name[256] = "";
    if (p){ if (!gok(p, 1)) { set_err(WSAEFAULT); return 0; }
            uint32_t k = 0; while (k < 255 && gok(p + k, 1) && MR8(p + k)){ name[k] = (char)MR8(p + k); k++; } name[k] = 0; }
    uint32_t ips[8]; int n = 0;
    struct in_addr a;
    if (own_name(name)){ n = own_addrs(ips); return make_hostent(c, g_ws_id.name, ips, n); }
    if (!strcasecmp(name, "localhost")){ ips[0] = mkip(127, 0, 0, 1); return make_hostent(c, g_ws_id.name, ips, 1); }
    if (inet_pton(AF_INET, name, &a) == 1){ ips[0] = a.s_addr; return make_hostent(c, name, ips, 1); }
    if (g_online && g_game.online_host && !strcasecmp(name, g_game.online_host)){   /* the server's online host (see the top) */
        static int said;
        if (!said++) ws_log("online: %s is the server's online host 10.77.0.1", g_game.online_host);
        ips[0] = mkip(10, 77, 0, 1); return make_hostent(c, g_game.online_host, ips, 1);
    }
    if (g_ws_mode != NET_LAN){
        st_blocked++;
        if (st_blocked <= 200) ws_log("BLOCKED dns \"%s\" (mode %s) from 0x%08x", name, mode_name(g_ws_mode), c->eip);
        set_err(WSAHOST_NOT_FOUND); return 0;
    }
#ifdef __wasm__
    return 0;                                             /* not reached: the browser has no lan mode */
#else
    /* lan: the system resolver, with the guest lock released */
    struct addrinfo hint, *res = 0; memset(&hint, 0, sizeof hint); hint.ai_family = AF_INET; hint.ai_flags = AI_CANONNAME;
    guest_wait_begin();
    int r = getaddrinfo(name, 0, &hint, &res);
    guest_wait_end();
    if (r){ set_err(r == EAI_AGAIN ? WSATRY_AGAIN : r == EAI_NONAME ? WSAHOST_NOT_FOUND : r == EAI_NODATA ? WSANO_DATA : WSANO_RECOVERY); return 0; }
    char canon[256]; snprintf(canon, sizeof canon, "%s", res && res->ai_canonname ? res->ai_canonname : name);
    for (struct addrinfo *q = res; q && n < 8; q = q->ai_next)
        if (q->ai_family == AF_INET){
            uint32_t v = ((struct sockaddr_in *)q->ai_addr)->sin_addr.s_addr; int d = 0;
            for (int i = 0; i < n; i++) d |= ips[i] == v;
            if (!d) ips[n++] = v; }
    freeaddrinfo(res);
    if (!n){ set_err(WSANO_DATA); return 0; }
    return make_hostent(c, canon, ips, n);
#endif
}
static uint32_t ws_gethostname(CPU *c){
    if (need_startup()) return 0xFFFFFFFFu;
    uint32_t p = A(c, 0); int len = (int)A(c, 1);
    size_t l = strlen(g_ws_id.name);
    if (!p || len <= (int)l || !gok(p, (uint32_t)l + 1)) FAIL(WSAEFAULT);
    memcpy(g_mem + p, g_ws_id.name, l + 1);
    return 0;
}
static uint32_t ws_inet_addr(CPU *c){
    uint32_t p = A(c, 0); char s[64] = "";
    if (!p || !gok(p, 1)) return 0xFFFFFFFFu;
    for (int k = 0; k < 63 && gok(p + (uint32_t)k, 1) && MR8(p + (uint32_t)k); k++){ s[k] = (char)MR8(p + (uint32_t)k); s[k + 1] = 0; }
    struct in_addr a;
    if (!*s) return 0xFFFFFFFFu;
    if (inet_aton(s, &a) != 1) return 0xFFFFFFFFu;     /* Winsock also accepts a, a.b, a.b.c forms */
    return a.s_addr;
}
static uint32_t ws_inet_ntoa(CPU *c){
    uint32_t ip = A(c, 0), b = gbuf(c) + 344;
    char t[16]; ipstr(ip, t); memcpy(g_mem + b, t, strlen(t) + 1);
    return b;
}

/* ======================================================================= */
/* netapi32!Netbios: NCBRESET / NCBASTAT / NCBENUM on one LANA              */
/* ======================================================================= */
#define NRC_GOODRET 0x00
#define NRC_BUFLEN  0x01
#define NRC_ILLCMD  0x03
#define NRC_INCOMP  0x06
#define NRC_BRIDGE  0x23
static uint32_t ws_netbios(CPU *c){
    ws_config();
    uint32_t ncb = A(c, 0);
    if (!gok(ncb, 64)) return NRC_BUFLEN;
    uint8_t cmd = MR8(ncb) & 0x7F, lana = MR8(ncb + 0x30);
    uint32_t buf = MR32(ncb + 4); uint16_t blen = MR16(ncb + 8);
    uint8_t rc = NRC_GOODRET;
    if (g_ws_mode == NET_VLAN && !g_vlan_up) vlan_up();
    switch (cmd){
    case 0x32:                                            /* NCBRESET */
        rc = lana == 0 ? NRC_GOODRET : NRC_BRIDGE; break;
    case 0x33: {                                          /* NCBASTAT */
        if (lana != 0){ rc = NRC_BRIDGE; break; }
        if (MR8(ncb + 0xA) != '*'){ rc = 0x05; break; }  /* remote adapter status: NRC_CMDTMO */
        uint8_t as[60]; memset(as, 0, sizeof as);
        memcpy(as, g_ws_id.mac, 6);
        as[6] = 3;  as[8] = 0xFE;                          /* rev_major 3, adapter_type Ethernet */
        uint16_t w[] = { 255, 255, 255, 0, 1464, 0, 64, 64, 1700, 0 };  /* free_ncbs .. name_count */
        for (int i = 0; i < 10; i++){ as[40 + 2 * i] = (uint8_t)w[i]; as[41 + 2 * i] = (uint8_t)(w[i] >> 8); }
        uint32_t n = blen < 60 ? blen : 60;
        if (n && !gok(buf, n)){ rc = NRC_BUFLEN; break; }
        if (n) memcpy(g_mem + buf, as, n);
        MR16(ncb + 8) = (uint16_t)n;
        rc = n < 60 ? NRC_INCOMP : NRC_GOODRET;
        break; }
    case 0x37:                                            /* NCBENUM: LANA_ENUM { length, lana[] } */
        if (blen < 2 || !gok(buf, 2)){ rc = NRC_BUFLEN; break; }
        MR8(buf) = 1; MR8(buf + 1) = 0; MR16(ncb + 8) = 2; break;
    default:
        { static int said; if (said++ < 8) ws_log("Netbios command 0x%02x not supported -> NRC_ILLCMD", cmd); }
        rc = NRC_ILLCMD; break;
    }
    if (MR8(ncb) & 0x80){ static int said; if (!said++) ws_log("Netbios ASYNCH: completed synchronously"); }
    MR8(ncb + 1) = rc; MR8(ncb + 0x31) = rc;
    return rc;
}

/* ======================================================================= */
/* dispatch (runtime/win32.c win32_call)                                    */
/* ======================================================================= */
static const struct { int ord; const char *name; } k_ords[] = {
    { 1, "accept" }, { 2, "bind" }, { 3, "closesocket" }, { 4, "connect" }, { 5, "getpeername" },
    { 6, "getsockname" }, { 7, "getsockopt" }, { 8, "htonl" }, { 9, "htons" }, { 10, "ioctlsocket" },
    { 11, "inet_addr" }, { 12, "inet_ntoa" }, { 13, "listen" }, { 14, "ntohl" }, { 15, "ntohs" },
    { 16, "recv" }, { 17, "recvfrom" }, { 18, "select" }, { 19, "send" }, { 20, "sendto" },
    { 21, "setsockopt" }, { 22, "shutdown" }, { 23, "socket" }, { 52, "gethostbyname" },
    { 57, "gethostname" }, { 111, "WSAGetLastError" }, { 112, "WSASetLastError" },
    { 115, "WSAStartup" }, { 116, "WSACleanup" }, { 151, "__WSAFDIsSet" }, { 0, 0 } };

int winsock_call(CPU *c, const char *dll, const char *n, uint32_t *out){
    int ws = !strncasecmp(dll, "ws2_32", 6) || !strncasecmp(dll, "wsock32", 7);
    int nb = !strncasecmp(dll, "netapi32", 8);
    if (!ws && !nb) return 0;
    if (nb){ if (strcmp(n, "Netbios")) return 0; g_curc = c; *out = ws_netbios(c); return 1; }
    if (n[0] == '#'){ int o = atoi(n + 1); n = 0;
        for (int i = 0; k_ords[i].name; i++) if (k_ords[i].ord == o) n = k_ords[i].name;
        if (!n) return 0; }
    ws_config();
    g_curc = c;
    uint32_t r;
    switch (n[0]){                                        /* cheap first-letter split before strcmp */
    case 'W':
        if (!strcmp(n, "WSAGetLastError")) r = get_err();
        else if (!strcmp(n, "WSASetLastError")){ set_err((int)A(c, 0)); r = 0; }
        else if (!strcmp(n, "WSAStartup")) r = ws_startup(c);
        else if (!strcmp(n, "WSACleanup")) r = ws_cleanup();
        else if (!strcmp(n, "WSAIoctl")) r = ws_wsaioctl(c);
        else return 0;
        break;
    case 'h': case 'n':
        if (!strcmp(n, "htons") || !strcmp(n, "ntohs")){ uint32_t v = A(c, 0) & 0xFFFF; r = (v >> 8 | v << 8) & 0xFFFF; }
        else if (!strcmp(n, "htonl") || !strcmp(n, "ntohl")) r = __builtin_bswap32(A(c, 0));
        else return 0;
        break;
    case 's':
        if (!strcmp(n, "socket")) r = ws_socket(c);
        else if (!strcmp(n, "send")) r = do_send(c, 0);
        else if (!strcmp(n, "sendto")) r = do_send(c, 1);
        else if (!strcmp(n, "select")) r = ws_select(c);
        else if (!strcmp(n, "setsockopt")) r = ws_setsockopt(c);
        else if (!strcmp(n, "shutdown")) r = ws_shutdown(c);
        else return 0;
        break;
    case 'r':
        if (!strcmp(n, "recv")) r = do_recv(c, 0);
        else if (!strcmp(n, "recvfrom")) r = do_recv(c, 1);
        else return 0;
        break;
    case 'b': if (strcmp(n, "bind")) return 0; r = ws_bind(c); break;
    case 'c':
        if (!strcmp(n, "connect")) r = ws_connect(c);
        else if (!strcmp(n, "closesocket")) r = ws_closesocket(c);
        else return 0;
        break;
    case 'l': if (strcmp(n, "listen")) return 0; r = ws_listen(c); break;
    case 'a': if (strcmp(n, "accept")) return 0; r = ws_accept(c); break;
    case 'i':
        if (!strcmp(n, "ioctlsocket")) r = ws_ioctlsocket(c);
        else if (!strcmp(n, "inet_addr")) r = ws_inet_addr(c);
        else if (!strcmp(n, "inet_ntoa")) r = ws_inet_ntoa(c);
        else return 0;
        break;
    case 'g':
        if (!strcmp(n, "getsockname")) r = ws_getname(c, 0);
        else if (!strcmp(n, "getpeername")) r = ws_getname(c, 1);
        else if (!strcmp(n, "getsockopt")) r = ws_getsockopt(c);
        else if (!strcmp(n, "gethostbyname")) r = ws_gethostbyname(c);
        else if (!strcmp(n, "gethostname")) r = ws_gethostname(c);
        else return 0;
        break;
    case '_':
        if (strcmp(n, "__WSAFDIsSet")) return 0;
        { uint32_t h = A(c, 0), set = A(c, 1); r = 0;
          if (set && gok(set, 4)){ uint32_t k = MR32(set); if (k > SEL_MAX) k = SEL_MAX;
              for (uint32_t i = 0; i < k && gok(set + 4 + 4 * i, 4); i++) if (MR32(set + 4 + 4 * i) == h){ r = 1; break; } } }
        break;
    default: return 0;
    }
    *out = r;
    deny_tick();                                          /* after the call: its own send or startup counts first */
    return 1;
}

void winsock_report(void){
    if (g_ws_mode < 0) return;
#ifdef __wasm__
    if (g_ws_mode == NET_VLAN) vlan_report();             /* the virtual LAN's own counters (net_lan.c) */
#endif
    fprintf(stderr, "  network (%s)      : %llu sockets, %llu sends (%llu bytes), %llu receives (%llu bytes), "
            "%llu selects, %llu waits, %llu BLOCKED\n", mode_name(g_ws_mode),
            (unsigned long long)st_sockets, (unsigned long long)st_sent, (unsigned long long)st_bytes_out,
            (unsigned long long)st_recv, (unsigned long long)st_bytes_in, (unsigned long long)st_select,
            (unsigned long long)st_waits, (unsigned long long)st_blocked);
}

/* tests/net_harness.c: start again with the current environment */
void winsock_test_reset(void){
    for (int i = 1; i < NSOCK; i++) if (g_s[i].used){ if (g_s[i].fd >= 0) close(g_s[i].fd); if (g_s[i].aux_fd > 0) close(g_s[i].aux_fd); memset(&g_s[i], 0, sizeof g_s[i]); }
    g_startups = 0; g_initted = 0; g_ws_mode = -1; g_vlan_up = 0; g_online = 0;
    st_sockets = st_sent = st_recv = st_bytes_out = st_bytes_in = st_blocked = st_select = st_waits = 0;
    memset(g_gbuf_teb, 0, sizeof g_gbuf_teb);
}
uint64_t winsock_blocked_count(void){ return st_blocked; }
