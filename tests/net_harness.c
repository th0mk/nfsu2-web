/* net_harness.c -- checks runtime/winsock.c (+ net_vlan.c) against the
 * documented Windows Winsock 2 / Netbios behaviour, without the game.
 *
 * The import handlers are called the way runtime/win32.c calls them: a CPU
 * whose esp points at the stdcall arguments (return address already popped),
 * fs_base = a TEB (WSAGetLastError lives at TEB+0x34), and the guest lock
 * held; guest_wait_begin/end below release and retake an emulated guest lock,
 * so the tests can also check that waiting calls let other threads run.
 *
 * Network safety: the harness forces NFSU2_NET_HARNESS=1, so the HLE can only
 * run in "off", "loopback" (127.0.0.1 only; anything else is BLOCKED) or
 * "vlan" (AF_UNIX to tools/vlanhub.py) mode.  The few plain host sockets the
 * tests open themselves (a fake peer) are bound to 127.0.0.1.  The "lan
 * identity" check only reads the interface list and the routing table.
 *
 *   clang -O1 -Iruntime -o out/net_harness tests/net_harness.c runtime/winsock.c \
 *         runtime/net_vlan.c runtime/mem.c
 *   out/net_harness [--vlan-hub <python> <tools/vlanhub.py>]   # exit 0 = all pass
 */
#include "cpu.h"
#include "game.h"
#include "winsock.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int winsock_call(CPU *c, const char *dll, const char *n, uint32_t *out);
void winsock_test_reset(void);
uint64_t winsock_blocked_count(void);

/* winsock.c overrides the lifted NetTick (sub_0074b730); not used here */
void sub_0074b730_orig(CPU *c){ (void)c; }
/* winsock.c's Online parts (g_game's Online host, online.c, olfilter.c) run
 * only with Online on, which it never is off the browser (the Mac's hub has
 * no online host): stand-ins, so that the harness links at -O0 too */
const GameOps g_game = { .id = "ug2", .exe_name = "SPEED2.EXE", .vroot = "C:\\NFSU2" };
void online_lobby_read(uint32_t link, const uint8_t *p, int n){ (void)link; (void)p; (void)n; }   /* online.c */
void olfilter_lobby_mark(uint32_t link, uint8_t *p, int n){ (void)link; (void)p; (void)n; }       /* olfilter.c */
void olfilter_lobby_sent(uint32_t link, uint8_t *p, int n){ (void)link; (void)p; (void)n; }
/* bundle.m's Local Network notice (a weak reference in winsock.c): counted */
static int g_notices;
void app_local_network_notice(void){ g_notices++; }
/* win32.c's per-thread last error (TEB+0x34), which winsock.c uses */
void win32_set_last_error(CPU *c, uint32_t e){
    if (c && c->fs_base && mem_is_committed(c->fs_base + 0x34u)) MR32(c->fs_base + 0x34u) = e;
}
uint32_t win32_get_last_error(CPU *c){
    return (c && c->fs_base && mem_is_committed(c->fs_base + 0x34u)) ? MR32(c->fs_base + 0x34u) : 0u;
}

/* ---- emulated guest lock ---- */
static pthread_mutex_t g_gil = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_releases;
void guest_wait_begin(void){ atomic_fetch_add(&g_releases, 1); pthread_mutex_unlock(&g_gil); }
void guest_wait_end(void){ pthread_mutex_lock(&g_gil); }

static int g_pass, g_fail;
static const char *g_section = "";
static void check(const char *what, int ok, const char *fmt, ...){
    char b[300] = "";
    if (fmt){ va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); }
    printf("  %s  %-58s %s\n", ok ? "PASS" : "FAIL", what, b);
    if (ok) g_pass++; else { g_fail++; fprintf(stderr, "FAILED [%s] %s %s\n", g_section, what, b); }
}
static void section(const char *s){ g_section = s; printf("%s\n", s); }

/* ---- guest threads ---- */
typedef struct { CPU cpu; uint32_t teb, stack; } Th;
static Th T[4];
static void th_init(Th *t, int i){
    memset(t, 0, sizeof *t);
    t->teb = 0x00C00000u + 0x20000u * (uint32_t)i;
    t->stack = 0x00D00000u + 0x10000u * (uint32_t)(i + 1);
    mem_commit(t->teb, 0x2000); memset(g_mem + t->teb, 0, 0x2000);
    MR32(t->teb + 0x18) = t->teb; MR32(t->teb + 0x24) = 1u + (uint32_t)i;
    t->cpu.fs_base = t->teb;
}
static uint32_t wcv(Th *t, const char *name, int n, va_list ap){
    CPU *c = &t->cpu;
    c->esp = t->stack - 0x400u;
    for (int i = 0; i < n; i++) MR32(c->esp + 4u * (uint32_t)i) = va_arg(ap, uint32_t);
    c->eip = 0x00C0FFEEu;
    uint32_t r = 0xDEADDEADu;
    const char *dll = !strcmp(name, "Netbios") ? "netapi32.dll" : "ws2_32.dll";
    if (!winsock_call(c, dll, name, &r)){ fprintf(stderr, "winsock_call does not handle %s\n", name); exit(3); }
    return r;
}
static uint32_t wc(Th *t, const char *name, int n, ...){
    va_list ap; va_start(ap, n); uint32_t r = wcv(t, name, n, ap); va_end(ap); return r;
}
#define W(...) wc(&T[0], __VA_ARGS__)
static uint32_t err0(void){ return wc(&T[0], "WSAGetLastError", 0); }
#define SE 0xFFFFFFFFu                         /* SOCKET_ERROR / INVALID_SOCKET */

/* ---- guest scratch memory ---- */
#define GB 0x00E00000u
static uint32_t g_bump = GB;
static uint32_t galloc(uint32_t n){ uint32_t p = (g_bump + 15u) & ~15u; g_bump = p + n; memset(g_mem + p, 0, n); return p; }
static uint32_t sa(const char *ip, uint16_t port){
    uint32_t p = galloc(16);
    struct in_addr a; inet_pton(AF_INET, ip, &a);
    MR16(p) = 2; MR8(p + 2) = (uint8_t)(port >> 8); MR8(p + 3) = (uint8_t)port; MR32(p + 4) = a.s_addr;
    memset(g_mem + p + 8, 0xA5, 8);            /* sin_zero garbage: Winsock ignores it, DirtySock stores data there */
    return p;
}
static uint32_t u32p(uint32_t v){ uint32_t p = galloc(4); MR32(p) = v; return p; }
static uint16_t sa_port(uint32_t p){ return (uint16_t)(MR8(p + 2) << 8 | MR8(p + 3)); }
static const char *sa_ip(uint32_t p, char *b){ const uint8_t *q = g_mem + p + 4; snprintf(b, 16, "%u.%u.%u.%u", q[0], q[1], q[2], q[3]); return b; }
static uint32_t fdset(int n, const uint32_t *h){
    uint32_t p = galloc(4 + 4u * (uint32_t)(n > 64 ? n : 64));
    MR32(p) = (uint32_t)n; for (int i = 0; i < n; i++) MR32(p + 4 + 4u * (uint32_t)i) = h[i];
    return p;
}
static uint32_t tv(int sec, int usec){ uint32_t p = galloc(8); MR32(p) = (uint32_t)sec; MR32(p + 4) = (uint32_t)usec; return p; }
static double now_s(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
static uint32_t gstr(const char *s){ uint32_t p = galloc((uint32_t)strlen(s) + 1); strcpy((char *)g_mem + p, s); return p; }

static void startup_mode(const char *mode){
    setenv("NFSU2_NET", mode, 1);
    winsock_test_reset();
}
static uint32_t udp_bound(const char *ip, uint16_t port){
    uint32_t s = W("socket", 3, 2, 2, 0);
    if (s == SE) return SE;
    uint32_t one = u32p(1);
    W("ioctlsocket", 3, s, 0x8004667Eu, one);
    W("setsockopt", 5, s, 0xFFFFu, 0x20u, one, 4u);
    if (W("bind", 3, s, sa(ip, port), 16u) != 0){ W("closesocket", 1, s); return SE; }
    return s;
}
static uint16_t local_port(uint32_t s){
    uint32_t a = galloc(16), l = u32p(16);
    if (W("getsockname", 3, s, a, l)) return 0;
    return sa_port(a);
}
static int wait_readable(Th *t, uint32_t s, int ms){
    uint32_t r = wc(t, "select", 5, 0u, fdset(1, &s), 0u, 0u, tv(ms / 1000, (ms % 1000) * 1000));
    return r == 1;
}

/* ======================================================================= */
static void t_startup(void){
    section("WSAStartup / WSACleanup / WSADATA");
    startup_mode("loopback");
    check("socket before WSAStartup -> WSANOTINITIALISED", W("socket", 3, 2, 2, 0) == SE && err0() == WSANOTINITIALISED, "err %u", err0());
    uint32_t wd = galloc(400);
    memset(g_mem + wd, 0xCC, 400);
    uint32_t r = W("WSAStartup", 2, 0x0002u, wd);
    check("WSAStartup(2.0) returns 0", r == 0, "r=%u", r);
    check("WSADATA.wVersion = 0x0002, wHighVersion = 0x0202", MR16(wd) == 0x0002 && MR16(wd + 2) == 0x0202, "%04x %04x", MR16(wd), MR16(wd + 2));
    check("szDescription at +4, szSystemStatus at +261", !strcmp((char *)g_mem + wd + 4, "WinSock 2.0") && !strcmp((char *)g_mem + wd + 261, "Running"), 0);
    check("iMaxSockets/iMaxUdpDg 0, lpVendorInfo NULL at +396", MR16(wd + 390) == 0 && MR16(wd + 392) == 0 && MR32(wd + 396) == 0, 0);
    check("the swapped wVersion passes DirtySock's >= 0x200 gate", ((MR16(wd) & 0xFF) << 8 | MR16(wd) >> 8) >= 0x200, 0);
    uint32_t wd2 = galloc(400);
    check("WSAStartup(2.2) -> wVersion 0x0202", W("WSAStartup", 2, 0x0202u, wd2) == 0 && MR16(wd2) == 0x0202, 0);
    check("WSAStartup(0.0) -> WSAVERNOTSUPPORTED", W("WSAStartup", 2, 0x0000u, wd2) == WSAVERNOTSUPPORTED, 0);
    check("WSAStartup(NULL) -> WSAEFAULT", W("WSAStartup", 2, 0x0002u, 0u) == WSAEFAULT, 0);
    uint32_t s = udp_bound("0.0.0.0", 0);
    check("first WSACleanup keeps sockets (reference counted)", W("WSACleanup", 0) == 0 && W("closesocket", 1, s) == 0, 0);
    s = W("socket", 3, 2, 2, 0);
    check("last WSACleanup returns 0", W("WSACleanup", 0) == 0, 0);
    check("... and closed the sockets: WSANOTINITIALISED now", W("closesocket", 1, s) == SE && err0() == WSANOTINITIALISED, "err %u", err0());
    W("WSAStartup", 2, 0x0002u, wd);
    check("after a new WSAStartup the old handle is gone (WSAENOTSOCK)", W("closesocket", 1, s) == SE && err0() == WSAENOTSOCK, "err %u", err0());
    check("WSACleanup without startup count -> SOCKET_ERROR", W("WSACleanup", 0) == 0 && W("WSACleanup", 0) == SE && err0() == WSANOTINITIALISED, 0);
    W("WSAStartup", 2, 0x0002u, wd);
}

static void t_sockets(void){
    section("socket handles, bind, sockaddr layout");
    uint32_t s1 = W("socket", 3, 2, 2, 0), s2 = W("socket", 3, 2, 1, 0), s3 = W("socket", 3, 2, 3, 1);
    check("socket(UDP), socket(TCP), socket(RAW, ICMP) succeed", s1 != SE && s2 != SE && s3 != SE, "%x %x %x", s1, s2, s3);
    check("SOCKETs are non-zero multiples of 4, all different", s1 && !(s1 & 3) && !(s2 & 3) && s1 != s2 && s2 != s3, 0);
    check("socket(AF_INET6=23) -> WSAEAFNOSUPPORT", W("socket", 3, 23, 2, 0) == SE && err0() == WSAEAFNOSUPPORT, 0);
    check("socket(RAW, TCP) -> WSAEPROTONOSUPPORT", W("socket", 3, 2, 3, 6) == SE && err0() == WSAEPROTONOSUPPORT, 0);
    check("socket(type 5) -> WSAESOCKTNOSUPPORT", W("socket", 3, 2, 5, 0) == SE && err0() == WSAESOCKTNOSUPPORT, 0);
    check("closesocket(bogus) -> WSAENOTSOCK", W("closesocket", 1, 0x1235u) == SE && err0() == WSAENOTSOCK, 0);
    check("bind with namelen 8 -> WSAEFAULT", W("bind", 3, s1, sa("0.0.0.0", 0), 8u) == SE && err0() == WSAEFAULT, 0);
    uint32_t bad = sa("0.0.0.0", 0); MR16(bad) = 23;
    check("bind AF_INET6 sockaddr -> WSAEAFNOSUPPORT", W("bind", 3, s1, bad, 16u) == SE && err0() == WSAEAFNOSUPPORT, 0);
    check("bind 0.0.0.0:47123", W("bind", 3, s1, sa("0.0.0.0", 47123), 16u) == 0, "err %u", err0());
    uint32_t a = galloc(16), l = u32p(99); memset(g_mem + a, 0xEE, 16);
    W("getsockname", 3, s1, a, l);
    check("getsockname: u16 family 2 (no BSD sin_len), port, 0.0.0.0", g_mem[a] == 2 && g_mem[a + 1] == 0 && sa_port(a) == 47123 && MR32(a + 4) == 0, "%02x %02x", g_mem[a], g_mem[a + 1]);
    check("getsockname: sin_zero cleared, namelen written 16", MR32(a + 8) == 0 && MR32(a + 12) == 0 && MR32(l) == 16, "len %u", MR32(l));
    check("getsockname namelen 15 -> WSAEFAULT", W("getsockname", 3, s1, a, u32p(15)) == SE && err0() == WSAEFAULT, 0);
    check("bind twice -> WSAEINVAL", W("bind", 3, s1, sa("0.0.0.0", 47124), 16u) == SE && err0() == WSAEINVAL, 0);
    uint32_t s4 = W("socket", 3, 2, 2, 0);
    check("second UDP bind to the same port -> WSAEADDRINUSE", W("bind", 3, s4, sa("0.0.0.0", 47123), 16u) == SE && err0() == WSAEADDRINUSE, "err %u", err0());
    check("getsockname on an unbound socket -> WSAEINVAL", W("getsockname", 3, s4, a, u32p(16)) == SE && err0() == WSAEINVAL, 0);
    check("shutdown(SD_BOTH) on an unconnected UDP socket -> 0 (Windows)", W("shutdown", 2, s1, 2u) == 0, 0);
    check("shutdown on an unconnected TCP socket -> WSAENOTCONN", W("shutdown", 2, s2, 2u) == SE && err0() == WSAENOTCONN, 0);
    check("listen on UDP -> WSAEOPNOTSUPP", W("listen", 2, s4, 5u) == SE && err0() == WSAEOPNOTSUPP, 0);
    check("listen on an unbound TCP socket -> WSAEINVAL", W("listen", 2, s2, 5u) == SE && err0() == WSAEINVAL, 0);
    check("accept on a non-listening socket -> WSAEINVAL", W("accept", 3, s2, 0u, 0u) == SE && err0() == WSAEINVAL, 0);
    check("getpeername on unconnected TCP -> WSAENOTCONN", W("getpeername", 3, s2, a, u32p(16)) == SE && err0() == WSAENOTCONN, 0);
    check("htons(0x1234) = 0x3412, htonl(0x11223344) = 0x44332211", W("htons", 1, 0x1234u) == 0x3412 && W("htonl", 1, 0x11223344u) == 0x44332211u, 0);
    check("ntohs, ntohl", W("ntohs", 1, 0xABCDu) == 0xCDAB && W("ntohl", 1, 0x01020304u) == 0x04030201u, 0);
    W("closesocket", 1, s1); W("closesocket", 1, s2); W("closesocket", 1, s3); W("closesocket", 1, s4);
}

static void t_udp(void){
    section("UDP: datagrams, WSAEMSGSIZE, WSAEWOULDBLOCK, broadcast, loopback guard");
    uint32_t r = udp_bound("0.0.0.0", 47200), s = udp_bound("0.0.0.0", 0);
    uint16_t sp = local_port(s);
    uint32_t buf = galloc(2048), from = galloc(16), fl = u32p(16);
    check("non-blocking recvfrom on an empty socket -> WSAEWOULDBLOCK", W("recvfrom", 6, r, buf, 2048u, 0u, from, fl) == SE && err0() == WSAEWOULDBLOCK, "err %u", err0());
    uint32_t msg = galloc(100); for (int i = 0; i < 100; i++) MR8(msg + (uint32_t)i) = (uint8_t)i;
    check("sendto own address 10.77.0.2 returns the length", W("sendto", 6, s, msg, 100u, 0u, sa("10.77.0.2", 47200), 16u) == 100, "err %u", err0());
    MR32(fl) = 16;
    uint32_t n = wait_readable(&T[0], r, 1000) ? W("recvfrom", 6, r, buf, 2048u, 0u, from, fl) : SE;
    char b[16];
    check("recvfrom gets it, from = 10.77.0.2:<sender port>", n == 100 && !memcmp(g_mem + buf, g_mem + msg, 100) && !strcmp(sa_ip(from, b), "10.77.0.2") && sa_port(from) == sp, "n=%u from %s:%u", n, b, sa_port(from));
    check("from: family 2, sin_zero 0, fromlen 16", MR16(from) == 2 && MR32(from + 8) == 0 && MR32(fl) == 16, 0);
    /* truncation */
    W("sendto", 6, s, msg, 100u, 0u, sa("10.77.0.2", 47200), 16u);
    uint32_t m2 = galloc(8); memcpy(g_mem + m2, "SECOND!!", 8);
    W("sendto", 6, s, m2, 8u, 0u, sa("10.77.0.2", 47200), 16u);
    wait_readable(&T[0], r, 1000);
    memset(g_mem + buf, 0, 64);
    n = W("recvfrom", 6, r, buf, 10u, 0u, from, fl);
    check("10-byte buffer for a 100-byte datagram -> WSAEMSGSIZE", n == SE && err0() == WSAEMSGSIZE, "n=%d err %u", (int)n, err0());
    check("... buffer holds the first 10 bytes", !memcmp(g_mem + buf, g_mem + msg, 10) && MR8(buf + 10) == 0, 0);
    wait_readable(&T[0], r, 1000);
    n = W("recvfrom", 6, r, buf, 2048u, 0u, from, fl);
    check("... and the rest is discarded: the next recv is the next datagram", n == 8 && !memcmp(g_mem + buf, "SECOND!!", 8), "n=%d", (int)n);
    /* MSG_PEEK */
    W("sendto", 6, s, m2, 8u, 0u, sa("10.77.0.2", 47200), 16u);
    wait_readable(&T[0], r, 1000);
    uint32_t nr = galloc(4);
    W("ioctlsocket", 3, r, 0x4004667Fu, nr);
    check("FIONREAD on UDP = size of the next datagram", MR32(nr) == 8, "%u", MR32(nr));
    n = W("recvfrom", 6, r, buf, 2048u, 2u, from, fl);
    uint32_t n2 = W("recvfrom", 6, r, buf, 2048u, 0u, from, fl);
    check("MSG_PEEK leaves the datagram queued", n == 8 && n2 == 8, 0);
    /* broadcast */
    uint32_t nb = W("socket", 3, 2, 2, 0);
    W("bind", 3, nb, sa("0.0.0.0", 0), 16u);
    check("broadcast without SO_BROADCAST -> WSAEACCES", W("sendto", 6, nb, msg, 10u, 0u, sa("255.255.255.255", 47200), 16u) == SE && err0() == WSAEACCES, "err %u", err0());
    /* A fake peer at 127.0.0.1:48999 = 10.77.0.3:47999 (NFSU2_NET_LOOPMAP set
     * in main).  Not 9999 itself: other loopback-mode game runs on this Mac
     * may hold 127.0.0.1:9999 (loopback mode has one IP for everyone). */
    int peer = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in pa = { sizeof pa, AF_INET, htons(48999), { htonl(INADDR_LOOPBACK) }, { 0 } };
    int pb = bind(peer, (struct sockaddr *)&pa, sizeof pa);
    uint32_t g9 = udp_bound("0.0.0.0", 47999);
    check("guest binds 0.0.0.0:47999 (host 127.0.0.1:47999), peer 127.0.0.1:48999", g9 != SE && pb == 0, "err %u, peer bind %d", err0(), pb);
    uint32_t q = galloc(384); memcpy(g_mem + q, "gEA\0\0\0\0\0?", 9);
    check("broadcast 255.255.255.255:47999 with SO_BROADCAST", W("sendto", 6, g9, q, 384u, 0u, sa("255.255.255.255", 47999), 16u) == 384, "err %u", err0());
    MR32(fl) = 16;
    n = wait_readable(&T[0], g9, 1000) ? W("recvfrom", 6, g9, buf, 2048u, 0u, from, fl) : SE;
    check("own copy arrives (Windows loops broadcasts back), from 10.77.0.2:47999", n == 384 && !strcmp(sa_ip(from, b), "10.77.0.2") && sa_port(from) == 47999, "n=%d %s:%u", (int)n, b, sa_port(from));
    uint8_t pbuf[512]; struct sockaddr_in pf; socklen_t pfl = sizeof pf;
    struct pollfd pp = { peer, POLLIN, 0 }; poll(&pp, 1, 1000);
    ssize_t pn = recvfrom(peer, pbuf, sizeof pbuf, MSG_DONTWAIT, (struct sockaddr *)&pf, &pfl);
    check("the mapped peer port gets the broadcast too", pn == 384 && !memcmp(pbuf, "gEA", 3), "pn=%zd", pn);
    struct sockaddr_in dst = { sizeof dst, AF_INET, htons(47999), { htonl(INADDR_LOOPBACK) }, { 0 } };
    sendto(peer, "gEA advert", 10, 0, (struct sockaddr *)&dst, sizeof dst);
    MR32(fl) = 16;
    n = wait_readable(&T[0], g9, 1000) ? W("recvfrom", 6, g9, buf, 2048u, 0u, from, fl) : SE;
    check("the peer's unicast reply appears from 10.77.0.3:47999", n == 10 && !strcmp(sa_ip(from, b), "10.77.0.3") && sa_port(from) == 47999, "n=%d %s:%u", (int)n, b, sa_port(from));
    check("unicast to the peer (10.77.0.3:47999 -> 127.0.0.1:48999)", W("sendto", 6, g9, msg, 5u, 0u, sa("10.77.0.3", 47999), 16u) == 5, 0);
    poll(&pp, 1, 1000); pn = recv(peer, pbuf, sizeof pbuf, MSG_DONTWAIT);
    check("... arrives at the peer", pn == 5, "pn=%zd", pn);
    close(peer);
    /* guard */
    uint64_t b0 = winsock_blocked_count();
    check("sendto 8.8.8.8:53 -> WSAENETUNREACH (BLOCKED)", W("sendto", 6, s, msg, 10u, 0u, sa("8.8.8.8", 53), 16u) == SE && err0() == WSAENETUNREACH, "err %u", err0());
    check("sendto 192.168.178.1:9999 -> WSAENETUNREACH (BLOCKED)", W("sendto", 6, s, msg, 10u, 0u, sa("192.168.178.1", 9999), 16u) == SE && err0() == WSAENETUNREACH, 0);
    check("UDP connect 24.1.1.1 (route probe) -> WSAENETUNREACH (BLOCKED)", W("connect", 3, nb, sa("24.1.1.1", 0), 16u) == SE && err0() == WSAENETUNREACH, 0);
    uint32_t bb = W("socket", 3, 2, 2, 0);
    check("bind 192.168.1.5 -> WSAEADDRNOTAVAIL (BLOCKED)", W("bind", 3, bb, sa("192.168.1.5", 0), 16u) == SE && err0() == WSAEADDRNOTAVAIL, 0);
    check("every refusal counted as BLOCKED", winsock_blocked_count() == b0 + 4, "%llu", (unsigned long long)(winsock_blocked_count() - b0));
    check("sendto the peer's unmapped port: silently lost, like a LAN", W("sendto", 6, s, msg, 10u, 0u, sa("10.77.0.3", 5555), 16u) == 10 && winsock_blocked_count() == b0 + 4, 0);
    /* connected UDP */
    uint32_t cu = W("socket", 3, 2, 2, 0);
    check("UDP connect to own address, then send() and getpeername", W("connect", 3, cu, sa("10.77.0.2", 47200), 16u) == 0 && W("send", 4, cu, msg, 7u, 0u) == 7, "err %u", err0());
    uint32_t pa2 = galloc(16);
    check("getpeername on connected UDP", W("getpeername", 3, cu, pa2, u32p(16)) == 0 && sa_port(pa2) == 47200, 0);
    wait_readable(&T[0], r, 1000);
    check("... the datagram arrives", W("recv", 4, r, buf, 2048u, 0u) == 7, 0);
    check("send on unconnected UDP -> WSAENOTCONN", W("send", 4, s, msg, 7u, 0u) == SE && err0() == WSAENOTCONN, 0);
    check("recvfrom on an unbound UDP socket -> WSAEINVAL", W("recvfrom", 6, bb, buf, 10u, 0u, 0u, 0u) == SE && err0() == WSAEINVAL, 0);
    uint32_t big = galloc(70000);
    check("sendto 65508 bytes -> WSAEMSGSIZE", W("sendto", 6, s, big, 65508u, 0u, sa("10.77.0.2", 47200), 16u) == SE && err0() == WSAEMSGSIZE, 0);
    uint32_t hs[] = { r, s, nb, g9, bb, cu };
    for (int i = 0; i < 6; i++) W("closesocket", 1, hs[i]);
}

static void t_tcp(void){
    section("TCP: non-blocking connect, select semantics, accept, close/reset, SIGPIPE");
    uint32_t one = u32p(1), w, x;
    /* refused */
    uint32_t c1 = W("socket", 3, 2, 1, 0);
    W("ioctlsocket", 3, c1, 0x8004667Eu, one);
    uint32_t r = W("connect", 3, c1, sa("10.77.0.2", 47391), 16u);
    check("non-blocking connect to a closed port -> WSAEWOULDBLOCK", r == SE && err0() == WSAEWOULDBLOCK, "err %u", err0());
    w = fdset(1, &c1); x = fdset(1, &c1);
    uint32_t rs = fdset(1, &c1);
    r = W("select", 5, 0u, rs, w, x, tv(1, 0));
    check("select: a failed connect is in exceptfds only (not readable/writable)", r == 1 && MR32(x) == 1 && MR32(w) == 0 && MR32(rs) == 0, "r=%d w=%u x=%u r=%u", (int)r, MR32(w), MR32(x), MR32(rs));
    uint32_t ov = galloc(4), ol = u32p(4);
    W("getsockopt", 5, c1, 0xFFFFu, 0x1007u, ov, ol);
    check("getsockopt(SO_ERROR) = WSAECONNREFUSED", MR32(ov) == WSAECONNREFUSED && MR32(ol) == 4, "%u", MR32(ov));
    MR32(ol) = 4; W("getsockopt", 5, c1, 0xFFFFu, 0x1007u, ov, ol);
    check("... and is cleared once read", MR32(ov) == 0, "%u", MR32(ov));
    /* the same set as write and except set, as server.dll passes it */
    uint32_t c2 = W("socket", 3, 2, 1, 0); W("ioctlsocket", 3, c2, 0x8004667Eu, one);
    W("connect", 3, c2, sa("10.77.0.2", 47391), 16u);
    uint32_t al = fdset(1, &c2);
    r = W("select", 5, 0u, 0u, al, al, tv(1, 0));
    check("aliased write/except set: count 1, the set ends up holding the socket", r == 1 && MR32(al) == 1 && MR32(al + 4) == c2, "r=%d n=%u", (int)r, MR32(al));
    /* success */
    uint32_t ls = W("socket", 3, 2, 1, 0);
    W("bind", 3, ls, sa("0.0.0.0", 0), 16u);
    uint16_t LP = local_port(ls);
    check("listen", W("listen", 2, ls, 5u) == 0, 0);
    W("ioctlsocket", 3, ls, 0x8004667Eu, one);
    uint32_t acc0 = galloc(16), accl = u32p(16);
    check("accept with nothing pending (non-blocking) -> WSAEWOULDBLOCK", W("accept", 3, ls, acc0, accl) == SE && err0() == WSAEWOULDBLOCK, 0);
    uint32_t c3 = W("socket", 3, 2, 1, 0); W("ioctlsocket", 3, c3, 0x8004667Eu, one);
    r = W("connect", 3, c3, sa("10.77.0.2", LP), 16u);
    check("non-blocking connect to a listener -> WSAEWOULDBLOCK", r == SE && err0() == WSAEWOULDBLOCK, 0);
        w = fdset(1, &c3); x = fdset(1, &c3);
    r = W("select", 5, 1u, 0u, w, x, tv(1, 0));
    check("select: a completed connect is writable, not in exceptfds", r == 1 && MR32(w) == 1 && MR32(x) == 0, "r=%d", (int)r);
    check("connect after completion -> WSAEISCONN", W("connect", 3, c3, sa("10.77.0.2", LP), 16u) == SE && err0() == WSAEISCONN, "err %u", err0());
    rs = fdset(1, &ls);
    check("select: the listener is readable", W("select", 5, 0u, rs, 0u, 0u, tv(1, 0)) == 1, 0);
    MR32(accl) = 16;
    uint32_t a = W("accept", 3, ls, acc0, accl);
    char b[16];
    check("accept -> new SOCKET, peer 10.77.0.2:<client port>", a != SE && !strcmp(sa_ip(acc0, b), "10.77.0.2") && sa_port(acc0) == local_port(c3), "%s:%u", b, sa_port(acc0));
    uint32_t pn = galloc(16);
    check("client getpeername = 10.77.0.2:<listener port>", W("getpeername", 3, c3, pn, u32p(16)) == 0 && sa_port(pn) == LP && !strcmp(sa_ip(pn, b), "10.77.0.2"), 0);
    uint32_t ln = galloc(16);
    check("connected getsockname = 10.77.0.2 (not 0.0.0.0)", W("getsockname", 3, c3, ln, u32p(16)) == 0 && !strcmp(sa_ip(ln, b), "10.77.0.2"), "%s", b);
    uint32_t msg = galloc(64); memcpy(g_mem + msg, "@dir hello", 10);
    check("send on the connection", W("send", 4, c3, msg, 10u, 0u) == 10, 0);
    uint32_t buf = galloc(256);
    W("ioctlsocket", 3, a, 0x8004667Eu, one);
    wait_readable(&T[0], a, 1000);
    uint32_t nr = galloc(4); W("ioctlsocket", 3, a, 0x4004667Fu, nr);
    check("FIONREAD on TCP = 10", MR32(nr) == 10, "%u", MR32(nr));
    check("recv on the accepted socket", W("recv", 4, a, buf, 256u, 0u) == 10 && !memcmp(g_mem + buf, "@dir hello", 10), 0);
    check("recv with nothing pending -> WSAEWOULDBLOCK", W("recv", 4, a, buf, 256u, 0u) == SE && err0() == WSAEWOULDBLOCK, 0);
    /* graceful close */
    W("closesocket", 1, c3);
    wait_readable(&T[0], a, 1000);
    check("peer closed gracefully: recv returns 0", W("recv", 4, a, buf, 256u, 0u) == 0, "err %u", err0());
    W("closesocket", 1, a);
    /* abortive close with the Windows 4-byte linger, passed with len 8 as server.dll does */
    uint32_t c4 = W("socket", 3, 2, 1, 0); W("ioctlsocket", 3, c4, 0x8004667Eu, one);
    W("connect", 3, c4, sa("10.77.0.2", LP), 16u);
    w = fdset(1, &c4); W("select", 5, 0u, 0u, w, 0u, tv(1, 0));
    wait_readable(&T[0], ls, 1000);
    MR32(accl) = 16; a = W("accept", 3, ls, acc0, accl);
    uint32_t lg = galloc(8); MR32(lg) = 1; MR32(lg + 4) = 0;     /* two dwords {1, 0}: Windows reads u_short {1, 0} */
    check("setsockopt(SO_LINGER, {1,0}, len 8)", W("setsockopt", 5, a, 0xFFFFu, 0x80u, lg, 8u) == 0, "err %u", err0());
    uint32_t lgo = galloc(8), lgl = u32p(8);
    W("getsockopt", 5, a, 0xFFFFu, 0x80u, lgo, lgl);
    check("getsockopt(SO_LINGER) -> 4 bytes {1, 0}", MR32(lgl) == 4 && MR16(lgo) == 1 && MR16(lgo + 2) == 0, "len %u", MR32(lgl));
    W("closesocket", 1, a);
    wait_readable(&T[0], c4, 1000);
    r = W("recv", 4, c4, buf, 256u, 0u);
    check("abortive close: the peer's recv -> WSAECONNRESET", r == SE && err0() == WSAECONNRESET, "r=%d err %u", (int)r, err0());
    w = fdset(1, &c4); x = fdset(1, &c4);
    r = W("select", 5, 0u, 0u, w, x, tv(0, 0));
    check("after a reset select says writable, not exceptional (as Windows)", r == 1 && MR32(w) == 1 && MR32(x) == 0, "r=%d w=%u x=%u", (int)r, MR32(w), MR32(x));
    check("after a reset getpeername -> WSAENOTCONN (DirtySock's disconnect test)", W("getpeername", 3, c4, pn, u32p(16)) == SE && err0() == WSAENOTCONN, "err %u", err0());
    signal(SIGPIPE, SIG_DFL);                     /* a SIGPIPE would kill the harness */
    int ok = 1; for (int i = 0; i < 4; i++){ r = W("send", 4, c4, msg, 10u, 0u); if (r != SE) ok = 0; }
    check("send after the reset fails (no SIGPIPE: the harness is alive)", ok && (err0() == WSAECONNRESET || err0() == WSAENOTCONN), "err %u", err0());
    W("closesocket", 1, c4); W("closesocket", 1, c1); W("closesocket", 1, c2);
    /* guard */
    uint32_t g = W("socket", 3, 2, 1, 0);
    check("TCP connect 192.168.1.1:80 -> WSAENETUNREACH (BLOCKED)", W("connect", 3, g, sa("192.168.1.1", 80), 16u) == SE && err0() == WSAENETUNREACH, 0);
    uint32_t g2 = W("socket", 3, 2, 1, 0); W("ioctlsocket", 3, g2, 0x8004667Eu, one);
    W("connect", 3, g2, sa("10.77.0.3", 1234), 16u);
    x = fdset(1, &g2);
    check("TCP connect to an unmapped peer port: refused (exceptfds)", W("select", 5, 0u, 0u, 0u, x, tv(0, 0)) == 1, 0);
    W("closesocket", 1, g); W("closesocket", 1, g2); W("closesocket", 1, ls);
}

static void t_select(void){
    section("select: > 64 sockets, errors, timeouts with the lock released");
    uint32_t h[100];
    for (int i = 0; i < 100; i++) h[i] = udp_bound("0.0.0.0", 0);
    uint32_t snd = udp_bound("0.0.0.0", 0), m = galloc(4);
    W("sendto", 6, snd, m, 4u, 0u, sa("10.77.0.2", local_port(h[77])), 16u);
    uint32_t set = fdset(100, h);
    uint32_t r = W("select", 5, 0u, set, 0u, 0u, tv(1, 0));
    check("100 sockets in one read set: 1 ready", r == 1 && MR32(set) == 1 && MR32(set + 4) == h[77], "r=%d n=%u", (int)r, MR32(set));
    check("__WSAFDIsSet", W("__WSAFDIsSet", 2, h[77], set) == 1 && W("__WSAFDIsSet", 2, h[3], set) == 0, 0);
    uint32_t es = fdset(0, h);
    check("all sets empty -> WSAEINVAL", W("select", 5, 0u, es, 0u, 0u, tv(0, 0)) == SE && err0() == WSAEINVAL, 0);
    check("all sets NULL -> WSAEINVAL", W("select", 5, 0u, 0u, 0u, 0u, tv(0, 0)) == SE && err0() == WSAEINVAL, 0);
    uint32_t bogus = 0x1003u, bs = fdset(1, &bogus);
    check("a non-socket in a set -> WSAENOTSOCK", W("select", 5, 0u, bs, 0u, 0u, tv(0, 0)) == SE && err0() == WSAENOTSOCK, 0);
    uint32_t ws = fdset(2, h);
    check("UDP sockets are writable", W("select", 5, 0u, 0u, ws, 0u, tv(0, 0)) == 2, 0);
    set = fdset(1, &h[5]);
    int rel0 = atomic_load(&g_releases);
    double t0 = now_s();
    r = W("select", 5, 0u, set, 0u, 0u, tv(0, 250000));
    double dt = now_s() - t0;
    check("timeout {0, 250000}: returns 0 after 0.25 s, set emptied", r == 0 && MR32(set) == 0 && dt > 0.22 && dt < 0.6, "r=%d %.3fs", (int)r, dt);
    check("... with the guest lock released while waiting", atomic_load(&g_releases) > rel0, 0);
    set = fdset(1, &h[5]); t0 = now_s();
    r = W("select", 5, 0u, set, 0u, 0u, tv(0, 0));
    check("timeout {0, 0} polls once", r == 0 && now_s() - t0 < 0.05, 0);
    for (int i = 0; i < 100; i++) W("closesocket", 1, h[i]);
    W("closesocket", 1, snd);
}

/* ---- threads: blocking calls ---- */
typedef struct { Th *t; uint32_t s, buf; uint32_t r, e; double dt; } BArg;
static void *blocking_recv(void *p){
    BArg *b = p;
    pthread_mutex_lock(&g_gil);
    double t0 = now_s();
    b->r = wc(b->t, "recvfrom", 6, b->s, b->buf, 256u, 0u, 0u, 0u);
    b->e = wc(b->t, "WSAGetLastError", 0);
    b->dt = now_s() - t0;
    pthread_mutex_unlock(&g_gil);
    return 0;
}
static void t_threads(void){
    section("threads: per-thread errors and hostent, blocking calls, closesocket wakes waiters");
    W("WSASetLastError", 1, 10035u);
    wc(&T[1], "WSASetLastError", 1, 10054u);
    check("WSAGetLastError is per thread (TEB+0x34)", err0() == 10035 && wc(&T[1], "WSAGetLastError", 0) == 10054 && MR32(T[0].teb + 0x34) == 10035, 0);
    uint32_t h0 = W("gethostbyname", 1, gstr("")), h1 = wc(&T[1], "gethostbyname", 1, gstr(""));
    check("gethostbyname: one hostent per thread", h0 && h1 && h0 != h1, "%x %x", h0, h1);
    uint32_t l0 = MR32(h0 + 12), a0 = MR32(l0);
    char b[16]; snprintf(b, 16, "%u.%u.%u.%u", MR8(a0), MR8(a0 + 1), MR8(a0 + 2), MR8(a0 + 3));
    check("hostent: h_addrtype 2, h_length 4, h_addr_list[0] = 10.77.0.2", MR16(h0 + 8) == 2 && MR16(h0 + 10) == 4 && !strcmp(b, "10.77.0.2") && MR32(l0 + 4) == 0, "%s", b);
    check("hostent: h_name = gethostname, empty alias list", !strcmp((char *)g_mem + MR32(h0), "NFSU2-LOOP") && MR32(MR32(h0 + 4)) == 0, 0);
    uint32_t hb = galloc(256);
    check("gethostname", W("gethostname", 2, hb, 256u) == 0 && !strcmp((char *)g_mem + hb, "NFSU2-LOOP"), 0);
    check("gethostname with a short buffer -> WSAEFAULT", W("gethostname", 2, hb, 5u) == SE && err0() == WSAEFAULT, 0);
    uint32_t hn = W("gethostbyname", 1, hb);
    check("gethostbyname(own name) answered locally", hn && MR32(MR32(MR32(hn + 12))) == MR32(a0), 0);
    hn = W("gethostbyname", 1, gstr("nfsu2-loop.local"));
    check("gethostbyname(own name + .local), any case: locally, no mDNS", hn != 0, 0);
    hn = W("gethostbyname", 1, gstr("localhost"));
    check("gethostbyname(localhost) = 127.0.0.1", hn && MR8(MR32(MR32(hn + 12))) == 127, 0);
    hn = W("gethostbyname", 1, gstr("10.1.2.3"));
    check("gethostbyname(dotted quad) without DNS", hn && MR32(MR32(MR32(hn + 12))) == inet_addr("10.1.2.3") && !strcmp((char *)g_mem + MR32(hn), "10.1.2.3"), 0);
    uint64_t b0 = winsock_blocked_count();
    hn = W("gethostbyname", 1, gstr("pcnfs05.ea.com"));
    check("gethostbyname(pcnfs05.ea.com) -> NULL, WSAHOST_NOT_FOUND, no lookup", !hn && err0() == WSAHOST_NOT_FOUND && winsock_blocked_count() == b0 + 1, 0);
    check("inet_addr / inet_ntoa", W("inet_addr", 1, gstr("1.2.3.4")) == inet_addr("1.2.3.4") && !strcmp((char *)g_mem + W("inet_ntoa", 1, inet_addr("5.6.7.8")), "5.6.7.8"), 0);
    /* blocking recv woken by data */
    uint32_t s = udp_bound("0.0.0.0", 0), zero = u32p(0);
    W("ioctlsocket", 3, s, 0x8004667Eu, zero);
    BArg ba = { &T[1], s, galloc(256), 0, 0, 0 };
    pthread_t th;
    int rel0 = atomic_load(&g_releases);
    pthread_mutex_unlock(&g_gil);
    pthread_create(&th, 0, blocking_recv, &ba);
    usleep(150000);
    pthread_mutex_lock(&g_gil);
    uint32_t snd = udp_bound("0.0.0.0", 0), m = gstr("wake");
    W("sendto", 6, snd, m, 4u, 0u, sa("10.77.0.2", local_port(s)), 16u);
    pthread_mutex_unlock(&g_gil);
    pthread_join(th, 0);
    pthread_mutex_lock(&g_gil);
    check("blocking recvfrom waits with the lock released, then gets the data", ba.r == 4 && ba.dt > 0.1 && atomic_load(&g_releases) > rel0, "r=%d %.3fs", (int)ba.r, ba.dt);
    /* blocking recv woken by closesocket from another thread */
    BArg bc = { &T[1], s, galloc(256), 0, 0, 0 };
    pthread_mutex_unlock(&g_gil);
    pthread_create(&th, 0, blocking_recv, &bc);
    usleep(150000);
    pthread_mutex_lock(&g_gil);
    double t0 = now_s();
    check("closesocket while another thread blocks in recvfrom", W("closesocket", 1, s) == 0, 0);
    pthread_mutex_unlock(&g_gil);
    pthread_join(th, 0);
    pthread_mutex_lock(&g_gil);
    check("... the blocked call returns SOCKET_ERROR / WSAEINTR within 0.2 s", bc.r == SE && bc.e == WSAEINTR && now_s() - t0 < 0.2, "r=%d e=%u %.3fs", (int)bc.r, bc.e, now_s() - t0);
    check("... and the handle is gone", W("closesocket", 1, s) == SE && err0() == WSAENOTSOCK, 0);
    /* SO_RCVTIMEO */
    s = udp_bound("0.0.0.0", 0); W("ioctlsocket", 3, s, 0x8004667Eu, zero);
    uint32_t to = u32p(150);
    W("setsockopt", 5, s, 0xFFFFu, 0x1006u, to, 4u);
    t0 = now_s();
    uint32_t r = W("recvfrom", 6, s, galloc(16), 16u, 0u, 0u, 0u);
    double dt = now_s() - t0;
    check("SO_RCVTIMEO 150 ms: blocking recv -> WSAETIMEDOUT after ~0.15 s", r == SE && err0() == WSAETIMEDOUT && dt > 0.13 && dt < 0.5, "%.3fs err %u", dt, err0());
    W("closesocket", 1, s); W("closesocket", 1, snd);
    /* blocking TCP connect + accept */
    uint32_t ls = W("socket", 3, 2, 1, 0);
    W("bind", 3, ls, sa("0.0.0.0", 0), 16u); W("listen", 2, ls, 2u);
    uint32_t c = W("socket", 3, 2, 1, 0);
    check("blocking connect returns 0 once connected", W("connect", 3, c, sa("10.77.0.2", local_port(ls)), 16u) == 0, "err %u", err0());
    uint32_t a = W("accept", 3, ls, 0u, 0u);
    check("blocking accept (NULL addr) returns the connection", a != SE, 0);
    uint32_t c2 = W("socket", 3, 2, 1, 0);
    r = W("connect", 3, c2, sa("10.77.0.2", 47396), 16u);
    check("blocking connect to a closed port -> WSAECONNREFUSED", r == SE && err0() == WSAECONNREFUSED, "err %u", err0());
    W("closesocket", 1, a); W("closesocket", 1, c); W("closesocket", 1, c2); W("closesocket", 1, ls);
}

static void t_ioctl_opts(void){
    section("WSAIoctl, ioctlsocket, socket options, raw ICMP");
    uint32_t s = W("socket", 3, 2, 2, 0);
    uint32_t out = galloc(16), ret = u32p(0);
    char b[16];
    uint32_t r = W("WSAIoctl", 9, s, 0xC8000014u, sa("24.1.1.1", 0), 16u, out, 16u, ret, 0u, 0u);
    check("SIO_ROUTING_INTERFACE_QUERY(24.1.1.1) = 10.77.0.2, 16 bytes", r == 0 && !strcmp(sa_ip(out, b), "10.77.0.2") && MR32(ret) == 16 && MR16(out) == 2, "%s", b);
    r = W("WSAIoctl", 9, s, 0xC8000014u, sa("159.153.253.82", 0), 16u, out, 16u, ret, 0u, 0u);
    check("... toward server.dll's %%bind address: 10.77.0.2, no traffic", r == 0 && !strcmp(sa_ip(out, b), "10.77.0.2"), 0);
    r = W("WSAIoctl", 9, s, 0xC8000014u, sa("127.0.0.1", 0), 16u, out, 16u, ret, 0u, 0u);
    check("... toward 127.0.0.1 = 127.0.0.1", r == 0 && !strcmp(sa_ip(out, b), "127.0.0.1"), 0);
    check("... with an 8-byte output buffer -> WSAEFAULT", W("WSAIoctl", 9, s, 0xC8000014u, sa("24.1.1.1", 0), 16u, out, 8u, ret, 0u, 0u) == SE && err0() == WSAEFAULT, 0);
    check("unknown WSAIoctl code -> WSAEOPNOTSUPP", W("WSAIoctl", 9, s, 0x12345678u, 0u, 0u, 0u, 0u, ret, 0u, 0u) == SE && err0() == WSAEOPNOTSUPP, 0);
    check("ioctlsocket(unknown) -> WSAEINVAL", W("ioctlsocket", 3, s, 0x1234u, u32p(0)) == SE && err0() == WSAEINVAL, 0);
    check("setsockopt(SO_BROADCAST) on TCP -> WSAENOPROTOOPT", W("setsockopt", 5, W("socket", 3, 2, 1, 0), 0xFFFFu, 0x20u, u32p(1), 4u) == SE && err0() == WSAENOPROTOOPT, 0);
    check("setsockopt(unknown level/option) -> WSAENOPROTOOPT", W("setsockopt", 5, s, 0xFFFFu, 0x7777u, u32p(1), 4u) == SE && err0() == WSAENOPROTOOPT, 0);
    uint32_t v = galloc(4), vl = u32p(4);
    W("setsockopt", 5, s, 0xFFFFu, 0x20u, u32p(1), 4u);
    check("getsockopt(SO_BROADCAST) = 1, SO_TYPE = 2", W("getsockopt", 5, s, 0xFFFFu, 0x20u, v, vl) == 0 && MR32(v) == 1 &&
          (MR32(vl) = 4, W("getsockopt", 5, s, 0xFFFFu, 0x1008u, v, vl) == 0) && MR32(v) == 2, 0);
    MR32(vl) = 1;
    check("getsockopt of a BOOL into a 1-byte buffer", W("getsockopt", 5, s, 0xFFFFu, 0x20u, v, vl) == 0 && MR32(vl) == 1, 0);
    check("setsockopt(IP_TTL 64)", W("setsockopt", 5, s, 0u, 4u, u32p(64), 4u) == 0, 0);
    check("setsockopt(SO_REUSEADDR) / SO_RCVBUF / TCP_NODELAY", W("setsockopt", 5, s, 0xFFFFu, 4u, u32p(1), 4u) == 0 &&
          W("setsockopt", 5, s, 0xFFFFu, 0x1002u, u32p(65536), 4u) == 0 &&
          W("setsockopt", 5, W("socket", 3, 2, 1, 0), 6u, 1u, u32p(1), 4u) == 0, 0);
    /* raw ICMP: the guest's ICMP message out, IP header + ICMP in */
    uint32_t ic = W("socket", 3, 2, 3, 1);
    W("setsockopt", 5, ic, 0u, 4u, u32p(64), 4u);
    uint32_t pk = galloc(24);
    uint8_t m[24] = { 8, 0, 0, 0, 0x12, 0x34, 0, 7, 's', 'P', 'S', 'g', 1, 2, 3, 4, 'X', 'X', 'X', 'X', 'X', 'X', 'X', 'X' };
    uint32_t sum = 0; for (int i = 0; i < 24; i += 2) sum += (uint32_t)(m[i] << 8 | m[i + 1]);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    sum = ~sum & 0xFFFF; m[2] = (uint8_t)(sum >> 8); m[3] = (uint8_t)sum;
    memcpy(g_mem + pk, m, 24);
    r = W("sendto", 6, ic, pk, 24u, 0u, sa("10.77.0.2", 0), 16u);
    check("raw ICMP echo request to own address (no root needed)", r == 24, "err %u", err0());
    uint32_t rb = galloc(0x424), from = galloc(16), fl = u32p(16);
    uint32_t n = wait_readable(&T[0], ic, 1000) ? W("recvfrom", 6, ic, rb, 0x424u, 0u, from, fl) : SE;
    int okh = n == 44 && MR8(rb) == 0x45 && (MR8(rb + 2) << 8 | MR8(rb + 3)) == 44 && MR8(rb + 9) == 1;
    check("reply: 20-byte IPv4 header first, total length 44 (wire format)", okh, "n=%d", (int)n);
    check("... ICMP type 0 at +0x14, magic 'sPSg' at +0x1C, id kept", n == 44 && MR8(rb + 0x14) == 0 && !memcmp(g_mem + rb + 0x1C, "sPSg", 4) && MR8(rb + 0x18) == 0x12 && MR8(rb + 0x19) == 0x34, 0);
    uint32_t cs = 0; for (int i = 0; i < 20; i += 2) cs += (uint32_t)(MR8(rb + (uint32_t)i) << 8 | MR8(rb + (uint32_t)i + 1));
    while (cs >> 16) cs = (cs & 0xFFFF) + (cs >> 16);
    check("... IP header checksum valid, from 10.77.0.2", cs == 0xFFFF && !strcmp(sa_ip(from, b), "10.77.0.2"), "%s", b);
    uint64_t b0 = winsock_blocked_count();
    check("ping 8.8.8.8 -> WSAENETUNREACH (BLOCKED)", W("sendto", 6, ic, pk, 24u, 0u, sa("8.8.8.8", 0), 16u) == SE && err0() == WSAENETUNREACH && winsock_blocked_count() == b0 + 1, 0);
    W("closesocket", 1, ic); W("closesocket", 1, s);
}

static void t_netbios(void){
    section("netapi32!Netbios");
    uint32_t ncb = galloc(64), buf = galloc(0xF0);
    memset(g_mem + ncb, 0, 64); MR8(ncb) = 0x32; MR8(ncb + 0x30) = 0;
    check("NCBRESET LANA 0 -> NRC_GOODRET", W("Netbios", 1, ncb) == 0 && MR8(ncb + 1) == 0 && MR8(ncb + 0x31) == 0, 0);
    MR8(ncb + 0x30) = 1;
    check("NCBRESET LANA 1 -> NRC_BRIDGE (one adapter)", W("Netbios", 1, ncb) == 0x23 && MR8(ncb + 1) == 0x23, 0);
    memset(g_mem + ncb, 0, 64); MR8(ncb) = 0x33; MR32(ncb + 4) = buf; MR16(ncb + 8) = 0xF0;
    memcpy(g_mem + ncb + 0xA, "*               ", 16);
    memset(g_mem + buf, 0xCC, 0xF0);
    uint32_t r = W("Netbios", 1, ncb);
    check("NCBASTAT \"*\" -> NRC_GOODRET, ncb_length 60", r == 0 && MR16(ncb + 8) == 60, "r=%u len=%u", r, MR16(ncb + 8));
    check("ADAPTER_STATUS: MAC 02:4e:56:4c:00:02 (vlan-style, never zero), Ethernet", !memcmp(g_mem + buf, "\x02\x4e\x56\x4c\x00\x02", 6) && MR8(buf + 8) == 0xFE, "%02x:%02x:%02x:%02x:%02x:%02x",
          MR8(buf), MR8(buf + 1), MR8(buf + 2), MR8(buf + 3), MR8(buf + 4), MR8(buf + 5));
    MR16(ncb + 8) = 10;
    check("NCBASTAT with a 10-byte buffer -> NRC_INCOMP, 10 bytes", W("Netbios", 1, ncb) == 0x06 && MR16(ncb + 8) == 10, 0);
    memset(g_mem + ncb, 0, 64); MR8(ncb) = 0x37; MR32(ncb + 4) = buf; MR16(ncb + 8) = 0xF0;
    check("NCBENUM -> one LANA (0)", W("Netbios", 1, ncb) == 0 && MR8(buf) == 1 && MR8(buf + 1) == 0, 0);
    memset(g_mem + ncb, 0, 64); MR8(ncb) = 0x10;
    check("unsupported command -> NRC_ILLCMD", W("Netbios", 1, ncb) == 0x03, 0);
}

static void t_modes(void){
    section("modes: off, harness refusal of lan, loopback address override");
    startup_mode("off");
    uint32_t wd = galloc(400);
    check("off: WSAStartup -> WSASYSNOTREADY", W("WSAStartup", 2, 0x0002u, wd) == WSASYSNOTREADY, 0);
    check("off: socket -> WSANOTINITIALISED", W("socket", 3, 2, 2, 0) == SE && err0() == WSANOTINITIALISED, 0);
    uint32_t ncb = galloc(64); MR8(ncb) = 0x32;
    check("off: Netbios still answers locally", W("Netbios", 1, ncb) == 0, 0);
    startup_mode("lan");                                      /* NFSU2_NET_HARNESS=1 is set */
    W("WSAStartup", 2, 0x0002u, wd);
    uint32_t s = W("socket", 3, 2, 2, 0), out = galloc(16), ret = u32p(0); char b[16];
    W("WSAIoctl", 9, s, 0xC8000014u, sa("24.1.1.1", 0), 16u, out, 16u, ret, 0u, 0u);
    check("lan under the harness becomes loopback (pseudo address 10.77.0.2)", !strcmp(sa_ip(out, b), "10.77.0.2"), "%s", b);
    check("... and still refuses a real destination", W("sendto", 6, s, galloc(4), 4u, 0u, sa("192.168.178.20", 9999), 16u) == SE && err0() == WSAENETUNREACH, 0);
    setenv("NFSU2_NET_ALLOW_LAN", "1", 1);
    startup_mode("lan");
    W("WSAStartup", 2, 0x0002u, wd);
    s = W("socket", 3, 2, 2, 0);
    W("WSAIoctl", 9, s, 0xC8000014u, sa("24.1.1.1", 0), 16u, out, 16u, ret, 0u, 0u);
    check("NFSU2_NET_ALLOW_LAN cannot lift NFSU2_NET_HARNESS", !strcmp(sa_ip(out, b), "10.77.0.2"), "%s", b);
    unsetenv("NFSU2_NET_ALLOW_LAN");
    unsetenv("NFSU2_NET_HARNESS"); unsetenv("ALARM");
    startup_mode("lan");
    W("WSAStartup", 2, 0x0002u, wd);
    s = W("socket", 3, 2, 2, 0);
    W("WSAIoctl", 9, s, 0xC8000014u, sa("24.1.1.1", 0), 16u, out, 16u, ret, 0u, 0u);
    check("lan without the harness flag but with ALARM unset (a debug run): loopback", !strcmp(sa_ip(out, b), "10.77.0.2"), "%s", b);
    setenv("ALARM", "30", 1);
    startup_mode("lan");
    W("WSAStartup", 2, 0x0002u, wd);
    s = W("socket", 3, 2, 2, 0);
    W("WSAIoctl", 9, s, 0xC8000014u, sa("24.1.1.1", 0), 16u, out, 16u, ret, 0u, 0u);
    check("... with ALARM=30 (a timed run): loopback", !strcmp(sa_ip(out, b), "10.77.0.2"), "%s", b);
    unsetenv("ALARM");
    setenv("NFSU2_NET_HARNESS", "1", 1);
    startup_mode("bogus");
    check("an unknown NFSU2_NET value means off", W("WSAStartup", 2, 0x0002u, wd) == WSASYSNOTREADY, 0);
    setenv("NFSU2_NET_ADDR", "10.77.0.9", 1);
    startup_mode("loopback");
    W("WSAStartup", 2, 0x0002u, wd);
    s = W("socket", 3, 2, 2, 0);
    W("WSAIoctl", 9, s, 0xC8000014u, sa("24.1.1.1", 0), 16u, out, 16u, ret, 0u, 0u);
    check("NFSU2_NET_ADDR picks the loopback guest address", !strcmp(sa_ip(out, b), "10.77.0.9"), "%s", b);
    unsetenv("NFSU2_NET_ADDR");
    setenv("NFSU2_NET_SIMULATE_DENY", "1", 1);
    startup_mode("loopback");
    W("WSAStartup", 2, 0x0002u, wd);
    { uint32_t one = u32p(1), c1 = W("socket", 3, 2, 1, 0); W("ioctlsocket", 3, c1, 0x8004667Eu, one);
      uint32_t r1 = W("connect", 3, c1, sa("10.77.0.3", 9900), 16u), e1 = err0();
      uint32_t xs = fdset(1, &c1), ov = galloc(4), ol = u32p(4);
      uint32_t r2 = W("select", 5, 0u, 0u, 0u, xs, tv(1, 0));
      W("getsockopt", 5, c1, 0xFFFFu, 0x1007u, ov, ol);
      check("denied Local Network (simulated): connect pending, then exceptfds + WSAEHOSTUNREACH", r1 == SE && e1 == WSAEWOULDBLOCK && r2 == 1 && MR32(ov) == WSAEHOSTUNREACH, "%u %u", e1, MR32(ov));
      uint32_t u = udp_bound("0.0.0.0", 0);
      check("... and a broadcast fails with WSAEHOSTUNREACH", W("sendto", 6, u, galloc(8), 8u, 0u, sa("255.255.255.255", 47999), 16u) == SE && err0() == WSAEHOSTUNREACH, "err %u", err0());
      check("... and a unicast datagram to the peer", W("sendto", 6, u, galloc(8), 8u, 0u, sa("10.77.0.3", 47999), 16u) == SE && err0() == WSAEHOSTUNREACH, "err %u", err0());
      check("... but not one to the own address", W("sendto", 6, u, galloc(8), 8u, 0u, sa("10.77.0.2", 47999), 16u) == 8u, "err %u", err0());
      W("closesocket", 1, c1); W("closesocket", 1, u); }
    /* a refusal that lasts: the app's notice, once; a unicast failure alone never */
    setenv("NFSU2_NET_SIMULATE_DENY", "EPERM", 1); setenv("NFSU2_NET_DENY_NOTICE_S", "1", 1);
    startup_mode("loopback");
    W("WSAStartup", 2, 0x0002u, wd);
    { uint32_t u = udp_bound("0.0.0.0", 0); int n0 = g_notices;
      check("EPERM variant: WSAEACCES", W("sendto", 6, u, galloc(8), 8u, 0u, sa("10.77.0.3", 47999), 16u) == SE && err0() == WSAEACCES, "err %u", err0());
      struct timespec ts = { 1, 200000000L }; nanosleep(&ts, 0);
      W("sendto", 6, u, galloc(8), 8u, 0u, sa("10.77.0.3", 47999), 16u);
      check("unicast failures for 1.2 s: no notice", g_notices == n0, "%d", g_notices - n0);
      W("sendto", 6, u, galloc(8), 8u, 0u, sa("255.255.255.255", 47999), 16u);
      W("sendto", 6, u, galloc(8), 8u, 0u, sa("255.255.255.255", 47999), 16u);
      check("a failing broadcast after that: the notice, once", g_notices == n0 + 1, "%d", g_notices - n0);
      W("closesocket", 1, u); }
    /* a joiner: one failing server-list query, then it only listens */
    startup_mode("loopback");
    W("WSAStartup", 2, 0x0002u, wd);
    { uint32_t u = udp_bound("0.0.0.0", 0), buf = galloc(64), from = galloc(16), fl = u32p(16), one = u32p(1);
      W("ioctlsocket", 3, u, 0x8004667Eu, one);
      int n0 = g_notices;
      W("sendto", 6, u, galloc(8), 8u, 0u, sa("255.255.255.255", 47999), 16u);
      W("recvfrom", 6, u, buf, 64u, 0u, from, fl);
      check("one failing broadcast, then only receiving: no notice yet", g_notices == n0, "%d", g_notices - n0);
      struct timespec ts = { 1, 200000000L }; nanosleep(&ts, 0);
      MR32(fl) = 16; W("recvfrom", 6, u, buf, 64u, 0u, from, fl);
      MR32(fl) = 16; W("recvfrom", 6, u, buf, 64u, 0u, from, fl);
      check("... 1.2 s later, still only receiving: the notice, once", g_notices == n0 + 1, "%d", g_notices - n0);
      W("closesocket", 1, u); }
    /* a joiner hearing the host while its sends are still refused: the
     * refused query is sent once more at the limit, refused again: the notice */
    setenv("NFSU2_NET_LOOPPEEROFFSET", "0", 1);         /* any other host port is "the peer" */
    startup_mode("loopback");
    W("WSAStartup", 2, 0x0002u, wd);
    { uint32_t u = udp_bound("0.0.0.0", 0), buf = galloc(64), from = galloc(16), fl = u32p(16), one = u32p(1);
      W("ioctlsocket", 3, u, 0x8004667Eu, one);
      int n0 = g_notices;
      W("sendto", 6, u, galloc(8), 8u, 0u, sa("255.255.255.255", 47999), 16u);
      int hs = socket(AF_INET, SOCK_DGRAM, 0);
      struct sockaddr_in d = { sizeof d, AF_INET, htons(local_port(u)), { htonl(INADDR_LOOPBACK) }, { 0 } };
      sendto(hs, "advert", 6, 0, (struct sockaddr *)&d, sizeof d);
      uint32_t n = wait_readable(&T[0], u, 1000) ? W("recvfrom", 6, u, buf, 64u, 0u, from, fl) : SE;
      char b[16];
      check("one failing broadcast, then a datagram from the peer arrives", n == 6 && !strcmp(sa_ip(from, b), "10.77.0.3"), "n=%d from %s", (int)n, b);
      struct timespec ts = { 1, 200000000L }; nanosleep(&ts, 0);
      MR32(fl) = 16; W("recvfrom", 6, u, buf, 64u, 0u, from, fl);
      MR32(fl) = 16; W("recvfrom", 6, u, buf, 64u, 0u, from, fl);
      check("... 1.2 s later the query goes again, refused again: the notice, once", g_notices == n0 + 1, "%d", g_notices - n0);
      /* ... and with its socket closed, nothing to send again: hearing the peer keeps it quiet */
      W("closesocket", 1, u);
      startup_mode("loopback");
      W("WSAStartup", 2, 0x0002u, wd);
      u = udp_bound("0.0.0.0", 0); W("ioctlsocket", 3, u, 0x8004667Eu, one);
      n0 = g_notices;
      W("sendto", 6, u, galloc(8), 8u, 0u, sa("255.255.255.255", 47999), 16u);
      uint32_t u2 = udp_bound("0.0.0.0", 0); W("ioctlsocket", 3, u2, 0x8004667Eu, one);
      W("closesocket", 1, u);
      struct sockaddr_in d2 = { sizeof d2, AF_INET, htons(local_port(u2)), { htonl(INADDR_LOOPBACK) }, { 0 } };
      sendto(hs, "advert", 6, 0, (struct sockaddr *)&d2, sizeof d2);
      MR32(fl) = 16; n = wait_readable(&T[0], u2, 1000) ? W("recvfrom", 6, u2, buf, 64u, 0u, from, fl) : SE;
      nanosleep(&ts, 0);
      MR32(fl) = 16; W("recvfrom", 6, u2, buf, 64u, 0u, from, fl);
      check("query socket closed, the peer heard: no notice after the limit", n == 6 && g_notices == n0, "n=%d %d", (int)n, g_notices - n0);
      close(hs); W("closesocket", 1, u2); }
    unsetenv("NFSU2_NET_LOOPPEEROFFSET");
    /* a refusal on one LAN visit says nothing about the next */
    startup_mode("loopback");
    W("WSAStartup", 2, 0x0002u, wd);
    { uint32_t u = udp_bound("0.0.0.0", 0), buf = galloc(64), from = galloc(16), fl = u32p(16), one = u32p(1);
      W("ioctlsocket", 3, u, 0x8004667Eu, one);
      int n0 = g_notices;
      W("sendto", 6, u, galloc(8), 8u, 0u, sa("255.255.255.255", 47999), 16u);
      W("closesocket", 1, u);
      W("WSACleanup", 0);
      struct timespec ts = { 1, 200000000L }; nanosleep(&ts, 0);
      uint32_t r = W("WSAStartup", 2, 0x0002u, wd);
      u = udp_bound("0.0.0.0", 0); W("ioctlsocket", 3, u, 0x8004667Eu, one);
      MR32(fl) = 16; W("recvfrom", 6, u, buf, 64u, 0u, from, fl);
      check("failing broadcast, WSACleanup, WSAStartup 1.2 s later: no notice", r == 0 && g_notices == n0, "r=%u %d", r, g_notices - n0);
      W("closesocket", 1, u); }
    /* a prompt answered Allow 300 ms after the joiner's only query: at the
     * limit the query goes again and works, so no notice */
    setenv("NFSU2_NET_SIMULATE_DENY", "EHOSTUNREACH:300", 1);
    startup_mode("loopback");
    W("WSAStartup", 2, 0x0002u, wd);
    { uint32_t u = udp_bound("0.0.0.0", 0), buf = galloc(64), from = galloc(16), fl = u32p(16), one = u32p(1);
      W("ioctlsocket", 3, u, 0x8004667Eu, one);
      int n0 = g_notices;
      uint32_t r = W("sendto", 6, u, galloc(8), 8u, 0u, sa("255.255.255.255", 47999), 16u);
      struct timespec ts = { 1, 200000000L }; nanosleep(&ts, 0);
      MR32(fl) = 16; W("recvfrom", 6, u, buf, 64u, 0u, from, fl);
      MR32(fl) = 16; W("recvfrom", 6, u, buf, 64u, 0u, from, fl);
      check("query refused for 300 ms, then only listening: sent again at the limit, no notice", r == SE && g_notices == n0, "r=%u %d", r, g_notices - n0);
      W("closesocket", 1, u); }
    setenv("NFSU2_NET_SIMULATE_DENY", "EPERM", 1);
    unsetenv("NFSU2_NET_DENY_NOTICE_S");
    /* a prompt answered after 300 ms */
    setenv("NFSU2_NET_SIMULATE_DENY", "EHOSTUNREACH:300", 1);
    startup_mode("loopback");
    W("WSAStartup", 2, 0x0002u, wd);
    { uint32_t u = udp_bound("0.0.0.0", 0);
      int e1 = W("sendto", 6, u, galloc(8), 8u, 0u, sa("255.255.255.255", 47999), 16u) == SE;
      struct timespec ts = { 0, 400000000L }; nanosleep(&ts, 0);
      check("denial with a time limit: refused, then allowed", e1 && W("sendto", 6, u, galloc(8), 8u, 0u, sa("255.255.255.255", 47999), 16u) == 8u, "err %u", err0());
      W("closesocket", 1, u); }
    unsetenv("NFSU2_NET_SIMULATE_DENY");
    setenv("NFSU2_NET_LOOPOFFSET", "7", 1);
    startup_mode("loopback");
    W("WSAStartup", 2, 0x0002u, wd);
    uint32_t u = udp_bound("0.0.0.0", 47610);
    check("NFSU2_NET_LOOPOFFSET: getsockname still shows the guest port", local_port(u) == 47610, "%u", local_port(u));
    int hs = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in d = { sizeof d, AF_INET, htons(47617), { htonl(INADDR_LOOPBACK) }, { 0 } };
    sendto(hs, "offset", 6, 0, (struct sockaddr *)&d, sizeof d);
    uint32_t buf = galloc(64), from = galloc(16), fl = u32p(16);
    uint32_t n = wait_readable(&T[0], u, 1000) ? W("recvfrom", 6, u, buf, 64u, 0u, from, fl) : SE;
    check("... the host port is guest port + 7 (127.0.0.1:47617)", n == 6, "n=%d", (int)n);
    uint32_t u2 = udp_bound("0.0.0.0", 0);
    W("sendto", 6, u2, buf, 6u, 0u, sa("10.77.0.2", 47610), 16u);
    MR32(fl) = 16;
    n = wait_readable(&T[0], u, 1000) ? W("recvfrom", 6, u, buf, 64u, 0u, from, fl) : SE;
    check("... own traffic: sender port seen as the guest port", n == 6 && sa_port(from) == local_port(u2), "%u %u", sa_port(from), local_port(u2));
    close(hs); W("closesocket", 1, u); W("closesocket", 1, u2);
    unsetenv("NFSU2_NET_LOOPOFFSET");
    startup_mode("loopback");
    W("WSAStartup", 2, 0x0002u, wd);
}

static void t_lan_identity(void){
    section("lan identity on this Mac (read-only: interface list and routing table)");
    NetIdent id; char why[160] = "";
    int ok = winsock_lan_identity(&id, why, sizeof why);
    char b[16]; const uint8_t *q = (const uint8_t *)&id.ip; snprintf(b, 16, "%u.%u.%u.%u", q[0], q[1], q[2], q[3]);
    printf("    chosen: %s %s (%s), host name %s, adapter %02x:%02x:%02x:%02x:%02x:%02x\n", id.ifname, ok ? b : "-", why, id.name,
           id.mac[0], id.mac[1], id.mac[2], id.mac[3], id.mac[4], id.mac[5]);
    int tun = !strncmp(id.ifname, "utun", 4) || !strncmp(id.ifname, "awdl", 4) || !strncmp(id.ifname, "llw", 3) || !strncmp(id.ifname, "lo", 2);
    check("no tunnel / loopback / AWDL interface as the LAN identity", !ok || !tun, "%s", id.ifname);
    check("address not loopback or link-local", !ok || (q[0] != 127 && !(q[0] == 169 && q[1] == 254)), 0);
    check("adapter MAC never all zero, unicast", (id.mac[0] | id.mac[1] | id.mac[2] | id.mac[3] | id.mac[4] | id.mac[5]) && !(id.mac[0] & 1), 0);
    check("host name: no domain, no spaces", id.name[0] && !strchr(id.name, '.') && !strchr(id.name, ' '), "%s", id.name);
    NetIdent id2; char w2[160] = ""; winsock_lan_identity(&id2, w2, sizeof w2);
    check("stable: the same answer twice", !memcmp(&id, &id2, sizeof id), 0);
    setenv("NFSU2_NET_MAC", "02:11:22:33:44:55", 1);
    winsock_lan_identity(&id2, w2, sizeof w2);
    check("NFSU2_NET_MAC override", !memcmp(id2.mac, "\x02\x11\x22\x33\x44\x55", 6), 0);
    unsetenv("NFSU2_NET_MAC");
    if (ok){
        setenv("NFSU2_NET_ADDR", b, 1);
        winsock_lan_identity(&id2, w2, sizeof w2);
        check("NFSU2_NET_ADDR = the chosen address: the same interface", id2.ip == id.ip && id2.ifindex == id.ifindex && !strcmp(id2.ifname, id.ifname), "%s", w2);
    }
    setenv("NFSU2_NET_ADDR", "192.0.2.10", 1);
    winsock_lan_identity(&id2, w2, sizeof w2);
    check("NFSU2_NET_ADDR not of this Mac: ignored, with a warning", id2.ip == id.ip && strstr(winsock_lan_warning(), "not an address of this Mac"), "%s", winsock_lan_warning());
    unsetenv("NFSU2_NET_ADDR");
}

/* The choice among several interfaces, on made-up interface lists (no
 * getifaddrs, no routing socket, nothing sent). */
static const char *ipof(uint32_t ip, char *b){ const uint8_t *q = (const uint8_t *)&ip; snprintf(b, 16, "%u.%u.%u.%u", q[0], q[1], q[2], q[3]); return b; }
static void lan_pick(NetIdent *id, char *why, size_t n){ why[0] = 0; winsock_lan_identity(id, why, n); }
static void t_lan_choice(void){
    section("lan identity: the choice among interfaces (made-up lists)");
    NetIdent id; char why[200], b[16];
    /* Wi-Fi (default route, private address) and Ethernet on another network */
    WsTestIf two[] = { { "en0", "192.168.1.20", "255.255.255.0", "06:aa:bb:cc:dd:ee" },
                       { "en5", "10.0.0.5", "255.255.255.0", "00:11:22:33:44:55" } };
    winsock_test_ifaces(two, 2, "en0");
    lan_pick(&id, why, sizeof why);
    check("default: the default route's interface", !strcmp(id.ifname, "en0") && id.ip == inet_addr("192.168.1.20") && id.ifindex == 100, "%s %s", ipof(id.ip, b), why);
    check("... a private (locally administered) MAC is not used as the adapter id", memcmp(id.mac, "\x06\xaa\xbb\xcc\xdd\xee", 6) != 0, 0);
    setenv("NFSU2_NET_ADDR", "10.0.0.5", 1);
    lan_pick(&id, why, sizeof why);
    check("lan_address on the other network: its interface (index, mask, MAC)", !strcmp(id.ifname, "en5") && id.ifindex == 101 &&
          id.ip == inet_addr("10.0.0.5") && id.mask == inet_addr("255.255.255.0") && !memcmp(id.mac, "\x00\x11\x22\x33\x44\x55", 6) &&
          !winsock_lan_warning()[0], "%s %s", id.ifname, why);
    setenv("NFSU2_NET_ADDR", "10.1.2.3", 1);
    lan_pick(&id, why, sizeof why);
    check("lan_address the Mac does not have: ignored, warning", !strcmp(id.ifname, "en0") && strstr(winsock_lan_warning(), "not an address of this Mac"), "%s", winsock_lan_warning());
    setenv("NFSU2_NET_ADDR", "not-an-ip", 1);
    lan_pick(&id, why, sizeof why);
    check("lan_address that is no IPv4 address: ignored, warning", !strcmp(id.ifname, "en0") && strstr(winsock_lan_warning(), "not an IPv4 address"), "%s", winsock_lan_warning());
    unsetenv("NFSU2_NET_ADDR");
    setenv("NFSU2_NET_IFACE", "en5", 1);
    lan_pick(&id, why, sizeof why);
    check("lan_interface = en5", !strcmp(id.ifname, "en5") && id.ip == inet_addr("10.0.0.5") && !winsock_lan_warning()[0], "%s", why);
    setenv("NFSU2_NET_ADDR", "192.168.1.20", 1);
    lan_pick(&id, why, sizeof why);
    check("lan_address on en0 with lan_interface = en5: en0, warning", !strcmp(id.ifname, "en0") && strstr(winsock_lan_warning(), "not on lan_interface"), "%s", winsock_lan_warning());
    unsetenv("NFSU2_NET_ADDR");
    setenv("NFSU2_NET_IFACE", "en9", 1);
    lan_pick(&id, why, sizeof why);
    check("lan_interface without an IPv4 address: automatic, loud warning", !strcmp(id.ifname, "en0") && strstr(winsock_lan_warning(), "has no IPv4 address"), "%s", winsock_lan_warning());
    unsetenv("NFSU2_NET_IFACE");
    uint32_t r = 0;
    check("route query towards a public address: the chosen interface's", winsock_test_route(inet_addr("159.153.253.82"), &r) == 0 && r == inet_addr("192.168.1.20"), "%s", ipof(r, b));
    check("... towards the other interface's subnet: that interface's", winsock_test_route(inet_addr("10.0.0.9"), &r) == 0 && r == inet_addr("10.0.0.5"), "%s", ipof(r, b));
    setenv("NFSU2_NET_ADDR", "10.0.0.5", 1);
    check("... with lan_address = 10.0.0.5: a public address gets 10.0.0.5 (server.cfg %bind)", winsock_test_route(inet_addr("159.153.253.82"), &r) == 0 && r == inet_addr("10.0.0.5"), "%s", ipof(r, b));
    unsetenv("NFSU2_NET_ADDR");
    /* two adapters on one subnet: the chosen one answers */
    WsTestIf same[] = { { "en0", "192.168.1.20", "255.255.255.0", 0 }, { "en5", "192.168.1.30", "255.255.255.0", 0 } };
    winsock_test_ifaces(same, 2, "en0");
    setenv("NFSU2_NET_IFACE", "en5", 1);
    check("a subnet two adapters share: the chosen adapter's address", winsock_test_route(inet_addr("192.168.1.40"), &r) == 0 && r == inet_addr("192.168.1.30"), "%s", ipof(r, b));
    unsetenv("NFSU2_NET_IFACE");
    check("... and the default one's without lan_interface", winsock_test_route(inet_addr("192.168.1.40"), &r) == 0 && r == inet_addr("192.168.1.20"), "%s", ipof(r, b));
    /* a self-assigned (169.254) address: a cable straight to a PC */
    WsTestIf ll[] = { { "en0", "192.168.1.20", "255.255.255.0", 0 }, { "en5", "169.254.7.9", "255.255.0.0", "00:11:22:33:44:55" } };
    winsock_test_ifaces(ll, 2, "en0");
    lan_pick(&id, why, sizeof why);
    check("self-assigned en5 next to Wi-Fi: Wi-Fi by default", !strcmp(id.ifname, "en0"), "%s", why);
    setenv("NFSU2_NET_IFACE", "en5", 1);
    lan_pick(&id, why, sizeof why);
    check("... lan_interface = en5 takes the self-assigned address", !strcmp(id.ifname, "en5") && id.ip == inet_addr("169.254.7.9") && strstr(why, "self-assigned") && !winsock_lan_warning()[0], "%s", why);
    unsetenv("NFSU2_NET_IFACE");
    setenv("NFSU2_NET_ADDR", "169.254.7.9", 1);
    lan_pick(&id, why, sizeof why);
    check("... so does lan_address = 169.254.7.9", !strcmp(id.ifname, "en5") && id.mask == inet_addr("255.255.0.0"), "%s", why);
    unsetenv("NFSU2_NET_ADDR");
    WsTestIf only[] = { { "en5", "169.254.7.9", "255.255.0.0", 0 } };
    winsock_test_ifaces(only, 1, "");
    lan_pick(&id, why, sizeof why);
    check("only a self-assigned address: it is used", !strcmp(id.ifname, "en5") && id.ip == inet_addr("169.254.7.9"), "%s", why);
    check("... and the route query answers with it", winsock_test_route(inet_addr("169.254.20.1"), &r) == 0 && r == inet_addr("169.254.7.9"), "%s", ipof(r, b));
    WsTestIf alias[] = { { "en0", "169.254.3.3", "255.255.0.0", 0 }, { "en0", "192.168.1.20", "255.255.255.0", 0 } };
    winsock_test_ifaces(alias, 2, "en0");
    lan_pick(&id, why, sizeof why);
    check("an interface with a self-assigned and a real address: the real one", id.ip == inet_addr("192.168.1.20"), "%s", ipof(id.ip, b));
    WsTestIf vpn[] = { { "utun3", "10.8.0.2", "255.255.255.0", 0 }, { "en0", "192.168.1.20", "255.255.255.0", 0 } };
    winsock_test_ifaces(vpn, 2, "utun3");
    lan_pick(&id, why, sizeof why);
    check("a VPN holding the default route: the Ethernet/Wi-Fi interface", !strcmp(id.ifname, "en0"), "%s", why);
    winsock_test_ifaces(0, 0, "");
    check("no interface at all: no address", winsock_lan_identity(&id, why, sizeof why) == 0 && winsock_test_route(inet_addr("24.1.1.1"), &r) == WSAENETUNREACH, "%s", why);
    winsock_test_ifaces(0, -1, 0);
}

/* ======================================================================= */
/* vlan: two processes on a hub                                             */
/* ======================================================================= */
static int rd_line(int fd, char *b, int n, int ms){
    int k = 0;
    while (k < n - 1){
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, ms) <= 0) break;
        if (read(fd, b + k, 1) != 1) break;
        if (b[k] == '\n') break;
        k++;
    }
    b[k] = 0; return k;
}
static void say(int fd, const char *s){ (void)!write(fd, s, strlen(s)); (void)!write(fd, "\n", 1); }

/* Instance B, in a child process: bind 9999, answer A's broadcast by unicast,
 * accept A's TCP connection and reset it, then report its identity. */
static int vlan_child(int to_parent, int from_parent){
    pthread_mutex_lock(&g_gil);
    uint32_t wd = galloc(400);
    if (W("WSAStartup", 2, 0x0002u, wd)) { say(to_parent, "ERR startup"); return 1; }
    uint32_t s = udp_bound("0.0.0.0", 9999);
    uint32_t ls = W("socket", 3, 2, 1, 0);
    W("bind", 3, ls, sa("0.0.0.0", 9900), 16u); W("listen", 2, ls, 4u);
    uint32_t hb = galloc(64); W("gethostname", 2, hb, 64u);
    uint32_t ncb = galloc(64), nb = galloc(64); MR8(ncb) = 0x33; MR32(ncb + 4) = nb; MR16(ncb + 8) = 60; MR8(ncb + 0xA) = '*';
    W("Netbios", 1, ncb);
    char msg[160];
    snprintf(msg, sizeof msg, "READY %s %02x%02x%02x%02x%02x%02x", (char *)g_mem + hb, MR8(nb), MR8(nb + 1), MR8(nb + 2), MR8(nb + 3), MR8(nb + 4), MR8(nb + 5));
    say(to_parent, msg);
    uint32_t buf = galloc(512), from = galloc(16), fl = u32p(16);
    pthread_mutex_unlock(&g_gil);
    char cmd[64];
    rd_line(from_parent, cmd, sizeof cmd, 30000);           /* "GO": A has broadcast */
    pthread_mutex_lock(&g_gil);
    uint32_t n = wait_readable(&T[0], s, 2000) ? W("recvfrom", 6, s, buf, 512u, 0u, from, fl) : SE;
    char b[16];
    snprintf(msg, sizeof msg, "GOT %d %s:%u", (int)n, sa_ip(from, b), sa_port(from));
    say(to_parent, msg);
    W("sendto", 6, s, gstr("advert"), 6u, 0u, from, 16u);
    rd_line(from_parent, cmd, sizeof cmd, 30000);           /* "TCP": A connects */
    uint32_t a = wait_readable(&T[0], ls, 3000) ? W("accept", 3, ls, galloc(16), u32p(16)) : SE;
    uint32_t one = u32p(1);
    if (a != SE) W("ioctlsocket", 3, a, 0x8004667Eu, one);
    n = (a != SE && wait_readable(&T[0], a, 2000)) ? W("recv", 4, a, buf, 512u, 0u) : SE;
    snprintf(msg, sizeof msg, "TCPGOT %d %.*s", (int)n, n == SE ? 0 : (int)n, (char *)g_mem + buf);
    say(to_parent, msg);
    W("send", 4, a, gstr("@dir ADDR=10.77.0.3"), 19u, 0u);
    rd_line(from_parent, cmd, sizeof cmd, 30000);           /* "RST" */
    uint32_t lg = galloc(4); MR16(lg) = 1; MR16(lg + 2) = 0;
    W("setsockopt", 5, a, 0xFFFFu, 0x80u, lg, 4u);
    W("closesocket", 1, a);
    say(to_parent, "RESET");
    rd_line(from_parent, cmd, sizeof cmd, 30000);           /* "BYE" */
    W("closesocket", 1, s); W("closesocket", 1, ls);
    W("WSACleanup", 0);
    pthread_mutex_unlock(&g_gil);
    return 0;
}

static void t_vlan(const char *py, const char *hubpy){
    section("vlan: two processes on tools/vlanhub.py");
    /* the hub socket path must fit sockaddr_un (104 bytes): $TMPDIR if short enough, else /tmp */
    char dir[120]; const char *td = getenv("TMPDIR");
    if (td && *td && strlen(td) < 60) snprintf(dir, sizeof dir, "%s%snfsu2vlanXXXXXX", td, td[strlen(td) - 1] == '/' ? "" : "/");
    else snprintf(dir, sizeof dir, "/tmp/nfsu2vlanXXXXXX");
    if (!mkdtemp(dir)){ check("temp dir", 0, 0); return; }
    int fail0 = g_fail;
    char sock[256], pcap[256], log[256];
    snprintf(sock, sizeof sock, "%s/hub.sock", dir); snprintf(pcap, sizeof pcap, "%s/hub.pcap", dir); snprintf(log, sizeof log, "%s/hub.log", dir);
    pid_t hub = fork();
    if (!hub){
        execl(py, py, "-I", hubpy, "--sock", sock, "--pcap", pcap, "--log", log, "--delay", "2", "--quit-after", "60", (char *)0);
        _exit(127);
    }
    for (int i = 0; i < 100 && access(sock, F_OK); i++) usleep(50000);
    check("hub started", access(sock, F_OK) == 0, "%s", sock);
    char vl[300]; snprintf(vl, sizeof vl, "vlan:%s", sock);
    int p2c[2], c2p[2]; (void)!pipe(p2c); (void)!pipe(c2p);
    pthread_mutex_unlock(&g_gil);
    pid_t ch = fork();
    if (!ch){
        close(p2c[1]); close(c2p[0]);
        setenv("NFSU2_NET", vl, 1); winsock_test_reset();
        g_bump = GB + 0x80000;                                /* own scratch range */
        _exit(vlan_child(c2p[1], p2c[0]));
    }
    close(p2c[0]); close(c2p[1]);
    pthread_mutex_lock(&g_gil);
    char line[200];
    pthread_mutex_unlock(&g_gil);
    rd_line(c2p[0], line, sizeof line, 8000);
    pthread_mutex_lock(&g_gil);
    char bname[64] = "", bmac[32] = "";
    sscanf(line, "READY %63s %31s", bname, bmac);
    check("instance B joined (its own name and MAC)", !strncmp(line, "READY", 5), "%s", line);
    startup_mode(vl);
    uint32_t wd = galloc(400);
    check("instance A: WSAStartup on the vlan", W("WSAStartup", 2, 0x0002u, wd) == 0, 0);
    uint32_t hb = galloc(64); W("gethostname", 2, hb, 64u);
    uint32_t ncb = galloc(64), nb = galloc(64); MR8(ncb) = 0x33; MR32(ncb + 4) = nb; MR16(ncb + 8) = 60; MR8(ncb + 0xA) = '*';
    W("Netbios", 1, ncb);
    char amac[32]; snprintf(amac, sizeof amac, "%02x%02x%02x%02x%02x%02x", MR8(nb), MR8(nb + 1), MR8(nb + 2), MR8(nb + 3), MR8(nb + 4), MR8(nb + 5));
    check("A and B have different host names and adapter MACs", strcmp((char *)g_mem + hb, bname) && strcmp(amac, bmac), "A %s %s, B %s %s", (char *)g_mem + hb, amac, bname, bmac);
    uint32_t s = W("socket", 3, 2, 2, 0), out = galloc(16), ret = u32p(0); char b[16];
    W("WSAIoctl", 9, s, 0xC8000014u, sa("24.1.1.1", 0), 16u, out, 16u, ret, 0u, 0u);
    char aip[16]; sa_ip(out, aip);
    check("A's route query answers its own vlan address", !strncmp(aip, "10.77.0.", 8) && strcmp(aip, "10.77.0.0"), "%s", aip);
    W("closesocket", 1, s);
    uint32_t a9 = udp_bound("0.0.0.0", 9999);
    uint32_t dup = W("socket", 3, 2, 2, 0);
    check("second bind of 9999 in A without SO_REUSEADDR -> WSAEADDRINUSE", W("bind", 3, dup, sa("0.0.0.0", 9999), 16u) == SE && err0() == WSAEADDRINUSE, "err %u", err0());
    W("closesocket", 1, dup);
    uint32_t q = galloc(384); memcpy(g_mem + q, "gEA\0\0\0\0\0?", 9);
    check("A broadcasts a 384-byte query to 255.255.255.255:9999", W("sendto", 6, a9, q, 384u, 0u, sa("255.255.255.255", 9999), 16u) == 384, "err %u", err0());
    uint32_t buf = galloc(512), from = galloc(16), fl = u32p(16);
    uint32_t n = wait_readable(&T[0], a9, 2000) ? W("recvfrom", 6, a9, buf, 512u, 0u, from, fl) : SE;
    check("A receives its own broadcast, from its own address", n == 384 && !strcmp(sa_ip(from, b), aip) && sa_port(from) == 9999, "n=%d %s", (int)n, b);
    pthread_mutex_unlock(&g_gil);
    say(p2c[1], "GO");
    rd_line(c2p[0], line, sizeof line, 5000);
    pthread_mutex_lock(&g_gil);
    char exp[64]; snprintf(exp, sizeof exp, "GOT 384 %s:9999", aip);
    check("B receives A's broadcast with A's address as source", !strcmp(line, exp), "%s", line);
    MR32(fl) = 16;
    n = wait_readable(&T[0], a9, 2000) ? W("recvfrom", 6, a9, buf, 512u, 0u, from, fl) : SE;
    char bip[16]; sa_ip(from, bip);
    check("B's unicast reply reaches A from B's address", n == 6 && !memcmp(g_mem + buf, "advert", 6) && strcmp(bip, aip) && !strncmp(bip, "10.77.0.", 8), "n=%d from %s", (int)n, bip);
    /* TCP */
    uint32_t one = u32p(1);
    uint32_t c = W("socket", 3, 2, 1, 0); W("ioctlsocket", 3, c, 0x8004667Eu, one);
    pthread_mutex_unlock(&g_gil); say(p2c[1], "TCP"); pthread_mutex_lock(&g_gil);
    uint32_t r = W("connect", 3, c, sa(bip, 9900), 16u);
    check("A: non-blocking connect to B:9900 -> WSAEWOULDBLOCK", r == SE && err0() == WSAEWOULDBLOCK, 0);
    uint32_t w = fdset(1, &c), x = fdset(1, &c);
    r = W("select", 5, 0u, 0u, w, x, tv(2, 0));
    check("... select: writable (connected), not in exceptfds", r == 1 && MR32(w) == 1 && MR32(x) == 0, "r=%d", (int)r);
    uint32_t pn = galloc(16), ln = galloc(16);
    W("getpeername", 3, c, pn, u32p(16)); W("getsockname", 3, c, ln, u32p(16));
    check("... getpeername = B:9900, getsockname = A's address", sa_port(pn) == 9900 && !strcmp(sa_ip(pn, b), bip) && !strcmp(sa_ip(ln, b), aip), 0);
    W("send", 4, c, gstr("@tic RC4+MD5-V2"), 15u, 0u);
    pthread_mutex_unlock(&g_gil); rd_line(c2p[0], line, sizeof line, 5000); pthread_mutex_lock(&g_gil);
    check("B accepted and received A's bytes", !strcmp(line, "TCPGOT 15 @tic RC4+MD5-V2"), "%s", line);
    n = wait_readable(&T[0], c, 2000) ? W("recv", 4, c, buf, 512u, 0u) : SE;
    check("A receives B's reply through the hub relay", n == 19 && !memcmp(g_mem + buf, "@dir ADDR=10.77.0.3", 19), "n=%d", (int)n);
    uint32_t c2 = W("socket", 3, 2, 1, 0); W("ioctlsocket", 3, c2, 0x8004667Eu, one);
    W("connect", 3, c2, sa(bip, 9901), 16u);
    x = fdset(1, &c2); w = fdset(1, &c2);
    r = W("select", 5, 0u, 0u, w, x, tv(2, 0));
    uint32_t ov = galloc(4), ol = u32p(4); W("getsockopt", 5, c2, 0xFFFFu, 0x1007u, ov, ol);
    check("connect to a port nobody listens on: exceptfds, SO_ERROR WSAECONNREFUSED", r == 1 && MR32(x) == 1 && MR32(w) == 0 && MR32(ov) == WSAECONNREFUSED, "r=%d err %u", (int)r, MR32(ov));
    uint32_t c3 = W("socket", 3, 2, 1, 0); W("ioctlsocket", 3, c3, 0x8004667Eu, one);
    W("connect", 3, c3, sa("10.77.0.200", 9900), 16u);
    x = fdset(1, &c3);
    r = W("select", 5, 0u, 0u, 0u, x, tv(5, 0));
    MR32(ol) = 4; W("getsockopt", 5, c3, 0xFFFFu, 0x1007u, ov, ol);
    check("connect to an absent host: exceptfds after ~3 s, WSAEHOSTUNREACH", r == 1 && MR32(ov) == WSAEHOSTUNREACH, "r=%d err %u", (int)r, MR32(ov));
    uint32_t c4 = W("socket", 3, 2, 1, 0);
    check("vlan: connect outside the subnet (192.168.1.1) cannot reach anything", W("connect", 3, c4, sa("192.168.1.1", 80), 16u) == SE, "err %u", err0());
    pthread_mutex_unlock(&g_gil); say(p2c[1], "RST"); rd_line(c2p[0], line, sizeof line, 5000); pthread_mutex_lock(&g_gil);
    wait_readable(&T[0], c, 2000);
    r = W("recv", 4, c, buf, 512u, 0u);
    check("B's abortive close (linger {1,0}): A's recv -> WSAECONNRESET", r == SE && err0() == WSAECONNRESET, "r=%d err %u", (int)r, err0());
    w = fdset(1, &c); x = fdset(1, &c);
    r = W("select", 5, 0u, 0u, w, x, tv(0, 0));
    check("... select says writable, not exceptional", r == 1 && MR32(w) == 1 && MR32(x) == 0, "r=%d", (int)r);
    check("... and A's getpeername -> WSAENOTCONN", W("getpeername", 3, c, pn, u32p(16)) == SE && err0() == WSAENOTCONN, 0);
    /* one process as server and client, as a hosting Mac: connection 1 is
     * answered and closed by the server (the redirector), connection 2 sends
     * before the server accepts it (the lobby login) */
    {
        uint32_t L = W("socket", 3, 2, 1, 0), one1 = u32p(1);
        W("ioctlsocket", 3, L, 0x8004667Eu, one1);
        W("bind", 3, L, sa(aip, 9910), 16u); W("listen", 2, L, 25u);
        for (int round = 0; round < 3; round++){
            uint32_t k1 = W("socket", 3, 2, 1, 0); W("ioctlsocket", 3, k1, 0x8004667Eu, one1);
            W("connect", 3, k1, sa(aip, 9910), 16u);
            uint32_t ww = fdset(1, &k1); W("select", 5, 0u, 0u, ww, 0u, tv(2, 0));
            W("send", 4, k1, gstr("@dir"), 4u, 0u);
            wait_readable(&T[0], L, 2000);
            uint32_t s1 = W("accept", 3, L, galloc(16), u32p(16));
            W("ioctlsocket", 3, s1, 0x8004667Eu, one1);
            wait_readable(&T[0], s1, 2000);
            uint32_t g1 = W("recv", 4, s1, buf, 512u, 0u);
            W("send", 4, s1, gstr("@dir ADDR"), 9u, 0u);
            W("shutdown", 2, s1, 2u); W("closesocket", 1, s1);
            wait_readable(&T[0], k1, 2000);
            uint32_t g2 = W("recv", 4, k1, buf, 512u, 0u);
            W("shutdown", 2, k1, 2u); W("closesocket", 1, k1);
            uint32_t k2 = W("socket", 3, 2, 1, 0); W("ioctlsocket", 3, k2, 0x8004667Eu, one1);
            W("connect", 3, k2, sa(aip, 9910), 16u);
            ww = fdset(1, &k2); W("select", 5, 0u, 0u, ww, 0u, tv(2, 0));
            W("send", 4, k2, gstr("addr12345678"), 12u, 0u);
            W("send", 4, k2, gstr("skey"), 4u, 0u);
            usleep(20000);
            /* close other sockets meanwhile (unix sockets that carried fds):
             * the kernel's unix-socket GC must not flush the connection that
             * waits in the listener's queue (the hub keeps its copy) */
            for (int j = 0; j < 3; j++){
                uint32_t u = W("socket", 3, 2, 2, 0); W("bind", 3, u, sa("0.0.0.0", 0), 16u); W("closesocket", 1, u);
                uint32_t t = W("socket", 3, 2, 1, 0); W("ioctlsocket", 3, t, 0x8004667Eu, one1);
                W("connect", 3, t, sa(aip, 9911), 16u); W("closesocket", 1, t);
            }
            wait_readable(&T[0], L, 2000);
            uint32_t s2 = W("accept", 3, L, galloc(16), u32p(16));
            W("ioctlsocket", 3, s2, 0x8004667Eu, one1);
            uint32_t rd = wait_readable(&T[0], s2, 2000);
            uint32_t g3 = W("recv", 4, s2, buf, 512u, 0u);
            check("same process: server closes conn 1, accepts conn 2 with data already sent", g1 == 4 && g2 == 9 && rd && g3 == 16,
                  "round %d: %d %d ready=%u got %d err %u", round, (int)g1, (int)g2, rd, (int)g3, err0());
            W("closesocket", 1, s2); W("closesocket", 1, k2);
        }
        W("closesocket", 1, L);
    }
    /* ICMP through the hub */
    uint32_t ic = W("socket", 3, 2, 3, 1);
    uint32_t pk = galloc(16); uint8_t m[16] = { 8, 0, 0, 0, 0xAB, 0xCD, 0, 1, 's', 'P', 'S', 'g', 9, 9, 9, 9 };
    uint32_t sm = 0; for (int i = 0; i < 16; i += 2) sm += (uint32_t)(m[i] << 8 | m[i + 1]);
    while (sm >> 16) sm = (sm & 0xFFFF) + (sm >> 16);
    sm = ~sm & 0xFFFF; m[2] = (uint8_t)(sm >> 8); m[3] = (uint8_t)sm; memcpy(g_mem + pk, m, 16);
    W("sendto", 6, ic, pk, 16u, 0u, sa(bip, 0), 16u);
    MR32(fl) = 16;
    n = wait_readable(&T[0], ic, 2000) ? W("recvfrom", 6, ic, buf, 512u, 0u, from, fl) : SE;
    check("ping B: echo reply with IP header, from B's address", n == 36 && MR8(buf) == 0x45 && MR8(buf + 20) == 0 && !memcmp(g_mem + buf + 28, "sPSg", 4) && !strcmp(sa_ip(from, b), bip), "n=%d", (int)n);
    pthread_mutex_unlock(&g_gil); say(p2c[1], "BYE");
    int st = 0; waitpid(ch, &st, 0);
    pthread_mutex_lock(&g_gil);
    check("instance B exited cleanly", WIFEXITED(st) && WEXITSTATUS(st) == 0, "status %d", st);
    W("closesocket", 1, a9); W("closesocket", 1, c); W("closesocket", 1, c2); W("closesocket", 1, c3); W("closesocket", 1, ic);
    W("WSACleanup", 0);
    kill(hub, SIGTERM); waitpid(hub, &st, 0);
    FILE *f = fopen(log, "r"); int adv = 0, lob = 0, jo = 0; char l2[600];
    while (f && fgets(l2, sizeof l2, f)){ if (strstr(l2, "ADVT ")) adv++; if (strstr(l2, "joined")) jo++; if (strstr(l2, "reset by")) lob++; }
    if (f) fclose(f);
    check("hub log: both joined, the query dissected, the reset seen", jo == 2 && adv >= 1 && lob == 1, "joined %d advt %d reset %d (%s)", jo, adv, lob, log);
    FILE *pc = fopen(pcap, "rb"); long psz = 0; if (pc){ fseek(pc, 0, SEEK_END); psz = ftell(pc); fclose(pc); }
    check("pcap written", psz > 400, "%ld bytes (%s)", psz, pcap);
    if (g_fail == fail0){ unlink(sock); unlink(pcap); unlink(log); rmdir(dir); }   /* kept for a look when a check failed */
    startup_mode("loopback");
}

/* ======================================================================= */
/* loopback with a second instance (NFSU2_NET_LOOPPEEROFFSET, loop_run.sh) */
/* ======================================================================= */
static void loop_peer_env(int me){
    setenv("NFSU2_NET_ADDR", me == 2 ? "10.77.0.2" : "10.77.0.3", 1);
    setenv("NFSU2_NET_LOOPPEER", me == 2 ? "10.77.0.3" : "10.77.0.2", 1);
    setenv("NFSU2_NET_LOOPOFFSET", me == 2 ? "23517" : "24517", 1);
    setenv("NFSU2_NET_LOOPPEEROFFSET", me == 2 ? "24517" : "23517", 1);
    startup_mode("loopback");
}
static uint16_t icmp_sum(const uint8_t *b, int n){
    uint32_t s = 0; for (int i = 0; i + 1 < n; i += 2) s += (uint32_t)(b[i] << 8 | b[i + 1]);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}
/* echo request (ICMP only, as DirtySock's SocketSendto passes it) */
static uint32_t echo_req(uint16_t id, uint16_t seq){
    uint32_t q = galloc(16); uint8_t *b = g_mem + q;
    b[0] = 8; b[1] = 0; b[4] = (uint8_t)(id >> 8); b[5] = (uint8_t)id; b[6] = (uint8_t)(seq >> 8); b[7] = (uint8_t)seq;
    memcpy(b + 8, "sPSgpeer", 8);
    uint16_t c = icmp_sum(b, 16); b[2] = (uint8_t)(c >> 8); b[3] = (uint8_t)c;
    return q;
}
/* Instance B (10.77.0.3): 9999 and 9900 bound; answer A's broadcast by
 * unicast, accept A's connection, ping A; report what it saw. */
static int loop_peer_child(int to_parent, int from_parent){
    pthread_mutex_lock(&g_gil);
    loop_peer_env(3);
    uint32_t wd = galloc(400);
    if (W("WSAStartup", 2, 0x0002u, wd)) { say(to_parent, "ERR startup"); pthread_mutex_unlock(&g_gil); return 1; }
    uint32_t s = udp_bound("0.0.0.0", 9999);
    uint32_t ls = W("socket", 3, 2, 1, 0);
    W("setsockopt", 5, ls, 0xFFFFu, 0x4u, u32p(1), 4u);   /* SO_REUSEADDR, as server.dll's listener */
    uint32_t lb = W("bind", 3, ls, sa("0.0.0.0", 9900), 16u), le = lb ? err0() : 0;
    uint32_t ll = W("listen", 2, ls, 4u), lle = ll ? err0() : 0;
    char cmd[64], msg[200], b1[16], b2[16], b3[16] = "-";
    if (s == SE || lb || ll) snprintf(msg, sizeof msg, "ERR udp %u bind %u/%u listen %u/%u", s, lb, le, ll, lle);
    else snprintf(msg, sizeof msg, "READY");
    say(to_parent, msg);
    uint32_t buf = galloc(512), from = galloc(16), fl = u32p(16), pa = galloc(16), pl = u32p(16);
    pthread_mutex_unlock(&g_gil);
    rd_line(from_parent, cmd, sizeof cmd, 30000);           /* "GO": A has broadcast and connects */
    pthread_mutex_lock(&g_gil);
    uint32_t n = wait_readable(&T[0], s, 2000) ? W("recvfrom", 6, s, buf, 512u, 0u, from, fl) : SE;
    W("sendto", 6, s, gstr("advert"), 6u, 0u, from, 16u);
    uint32_t a = wait_readable(&T[0], ls, 3000) ? W("accept", 3, ls, pa, pl) : SE;
    uint32_t r = W("socket", 3, 2, 3, 1), got = SE;
    if (r != SE){
        W("sendto", 6, r, echo_req(0x4e53, 77), 16u, 0u, sa("10.77.0.2", 0), 16u);
        uint32_t f2 = galloc(16), l2 = u32p(16);
        for (int k = 0; k < 4 && got == SE; k++)
            if (wait_readable(&T[0], r, 1000)){
                uint32_t m = W("recvfrom", 6, r, buf, 512u, 0u, f2, l2);
                if (m != SE && m >= 28 && MR8(buf + 20) == 0 && MR8(buf + 26) == 0 && MR8(buf + 27) == 77){
                    got = m; snprintf(b3, sizeof b3, "%s", sa_ip(f2, cmd));
                    if (memcmp(g_mem + buf + 12, g_mem + f2 + 4, 4)) snprintf(b3, sizeof b3, "hdr-mismatch");
                }
                MR32(l2) = 16;
            }
        W("closesocket", 1, r);
    }
    snprintf(msg, sizeof msg, "GOT %d %s:%u ACC %s ECHO %d %s", (int)n, sa_ip(from, b1), sa_port(from),
             a != SE ? sa_ip(pa, b2) : "-", (int)got, b3);
    say(to_parent, msg);
    pthread_mutex_unlock(&g_gil);
    rd_line(from_parent, cmd, sizeof cmd, 30000);           /* "BYE" */
    pthread_mutex_lock(&g_gil);
    if (a != SE) W("closesocket", 1, a);
    W("closesocket", 1, s); W("closesocket", 1, ls);
    W("WSACleanup", 0);
    pthread_mutex_unlock(&g_gil);
    return 0;
}
static void t_loop_peer(void){
    section("loopback: two instances on 127.0.0.1 (NFSU2_NET_LOOPPEEROFFSET)");
    int p2c[2], c2p[2]; (void)!pipe(p2c); (void)!pipe(c2p);
    pthread_mutex_unlock(&g_gil);
    pid_t ch = fork();
    if (!ch){
        close(p2c[1]); close(c2p[0]);
        g_bump = GB + 0x80000;                                /* own scratch range */
        _exit(loop_peer_child(c2p[1], p2c[0]));
    }
    close(p2c[0]); close(c2p[1]);
    char line[200];
    rd_line(c2p[0], line, sizeof line, 8000);
    pthread_mutex_lock(&g_gil);
    check("instance B (10.77.0.3) bound 9999 and 9900", !strcmp(line, "READY"), "%s", line);
    loop_peer_env(2);
    uint32_t wd = galloc(400);
    W("WSAStartup", 2, 0x0002u, wd);
    uint32_t u = udp_bound("0.0.0.0", 0);
    uint32_t q = galloc(384); memcpy(g_mem + q, "gEA\0\0\0\0\0?", 9);
    check("A broadcasts to 255.255.255.255:9999", W("sendto", 6, u, q, 384u, 0u, sa("255.255.255.255", 9999), 16u) == 384, "err %u", err0());
    uint32_t c = W("socket", 3, 2, 1, 0), one = u32p(1);
    W("ioctlsocket", 3, c, 0x8004667Eu, one);
    uint32_t cr = W("connect", 3, c, sa("10.77.0.3", 9900), 16u);
    check("A connects to B's 9900 (non-blocking: WSAEWOULDBLOCK)", cr == SE && err0() == WSAEWOULDBLOCK, "err %u", err0());
    pthread_mutex_unlock(&g_gil);
    say(p2c[1], "GO");
    rd_line(c2p[0], line, sizeof line, 12000);
    pthread_mutex_lock(&g_gil);
    char bport[16]; snprintf(bport, sizeof bport, ":%u ", local_port(u));
    check("B gets A's broadcast from 10.77.0.2, A's guest port", !strncmp(line, "GOT 384 10.77.0.2:", 18) && strstr(line, bport), "%s (A port %u)", line, local_port(u));
    check("B's accept shows A's address 10.77.0.2 (an ephemeral port of the peer)", strstr(line, "ACC 10.77.0.2 ") != 0, "%s", line);
    check("B's ping of A: the reply comes from 10.77.0.2 (address and IP header)", strstr(line, "ECHO 36 10.77.0.2") || strstr(line, "ECHO 28 10.77.0.2"), "%s", line);
    uint32_t buf = galloc(512), from = galloc(16), fl = u32p(16); char b[16];
    uint32_t n = wait_readable(&T[0], u, 2000) ? W("recvfrom", 6, u, buf, 512u, 0u, from, fl) : SE;
    check("B's unicast answer reaches A from 10.77.0.3:9999", n == 6 && !strcmp(sa_ip(from, b), "10.77.0.3") && sa_port(from) == 9999, "n=%d %s:%u", (int)n, b, sa_port(from));
    uint32_t w = W("select", 5, 0u, 0u, fdset(1, &c), 0u, tv(1, 0));
    uint32_t pa = galloc(16), pl = u32p(16);
    check("A's connection completes; getpeername 10.77.0.3:9900", w == 1 && W("getpeername", 3, c, pa, pl) == 0 && !strcmp(sa_ip(pa, b), "10.77.0.3") && sa_port(pa) == 9900, "w=%u %s:%u", w, b, sa_port(pa));
    pthread_mutex_unlock(&g_gil);
    say(p2c[1], "BYE");
    int st = 0; waitpid(ch, &st, 0);
    pthread_mutex_lock(&g_gil);
    check("instance B exited cleanly", WIFEXITED(st) && WEXITSTATUS(st) == 0, "status %d", st);
    W("closesocket", 1, u); W("closesocket", 1, c);
    W("WSACleanup", 0);
    unsetenv("NFSU2_NET_ADDR"); unsetenv("NFSU2_NET_LOOPPEER"); unsetenv("NFSU2_NET_LOOPOFFSET"); unsetenv("NFSU2_NET_LOOPPEEROFFSET");
    startup_mode("loopback");
    W("WSAStartup", 2, 0x0002u, galloc(400));
}

int main(int argc, char **argv){
    setenv("NFSU2_NET_HARNESS", "1", 1);                  /* never the real network */
    unsetenv("NFSU2_NET_ADDR"); unsetenv("NFSU2_NET_MAC"); unsetenv("NFSU2_NET_IFACE"); unsetenv("NFSU2_NET_NAME");
    setenv("NFSU2_NET_LOOPMAP", "47999=48999,9900=19900,3658=13658", 1);
    setvbuf(stdout, 0, _IONBF, 0);
    mem_init();
    mem_commit(GB, 0x100000);
    for (int i = 0; i < 4; i++){ th_init(&T[i], i); mem_commit(T[i].stack - 0x10000u, 0x10000u); }
    pthread_mutex_lock(&g_gil);
    const char *only = getenv("NET_HARNESS_ONLY");
    if (!only || strstr(only, "startup")) t_startup();
    else { startup_mode("loopback"); W("WSAStartup", 2, 0x0002u, galloc(400)); }
    if (!only || strstr(only, "sock")) t_sockets();
    if (!only || strstr(only, "udp")) t_udp();
    if (!only || strstr(only, "tcp")) t_tcp();
    if (!only || strstr(only, "select")) t_select();
    if (!only || strstr(only, "thread")) t_threads();
    if (!only || strstr(only, "ioctl")) t_ioctl_opts();
    if (!only || strstr(only, "netbios")) t_netbios();
    if (!only || strstr(only, "mode")) t_modes();
    if (!only || strstr(only, "ident")) t_lan_identity();
    if (!only || strstr(only, "ident") || strstr(only, "choice")) t_lan_choice();
    if (!only || strstr(only, "looppeer")) t_loop_peer();
    if (argc >= 4 && !strcmp(argv[1], "--vlan-hub") && (!only || strstr(only, "vlan"))) t_vlan(argv[2], argv[3]);
    else printf("vlan: skipped (needs --vlan-hub <python> <tools/vlanhub.py>)\n");
    pthread_mutex_unlock(&g_gil);
    printf("net_harness: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
