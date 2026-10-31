/* roam_bsd.c -- free-roam multiplayer: BSD-socket platform layer (macOS).
 * See roam_plat.h.  Host sockets only: this never goes through the guest's
 * Winsock HLE. */
#include "roam_plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

static int             g_fd = -1;
static pthread_t       g_thr;
static int             g_thr_on;
static atomic_int      g_stop;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static rc_session     *g_s;
static roam_plat_cfg   g_cfg;
static roam_plat_stats g_st;          /* written under g_mu */

void roam_plat_lock(void){ pthread_mutex_lock(&g_mu); }
void roam_plat_unlock(void){ pthread_mutex_unlock(&g_mu); }
int64_t roam_plat_now_us(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
uint32_t roam_plat_random(void){ uint32_t r; do r = arc4random(); while (!r); return r; }
void roam_plat_notice(const char *name, const char *what){ fprintf(stderr, "[roam] notice: %s%s\n", name ? name : "", what); }
int roam_plat_lobby(const char *json){ (void)json; return 0; }      /* no panel: the game's top line */
int roam_plat_solid(void){ return -1; }                              /* no menu: the setting */
int roam_plat_quickchat(void){ return 0; }                           /* no page: no picker and no lines */
int roam_plat_say_pick(void){ return -1; }
int roam_plat_said(const char *json){ (void)json; return 1; }
int roam_plat_friend(const char *json){ (void)json; return 0; }      /* no page: no friends */
void roam_plat_say_marks(const uint32_t *ids, const float *xys, int n){ (void)ids; (void)xys; (void)n; }
uint32_t roam_plat_crews(uint32_t crews[RC_CREW_MAX], uint32_t rgb[RC_CREW_MAX]){ (void)crews; (void)rgb; return 0; }   /* the settings */
uint32_t roam_plat_room(char name[64]){ (void)name; return 0; }                                                   /* the settings */
const char *roam_ip_str(uint32_t ip, char *b){
    snprintf(b, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255u, (ip >> 8) & 255u, ip & 255u); return b; }
void roam_plat_get_stats(roam_plat_stats *o){ roam_plat_lock(); *o = g_st; roam_plat_unlock(); }

/* a test or debug run: loopback only, no LAN discovery (the rule LAN play
 * uses too, nettest.h) */
#include "nettest.h"
int roam_plat_test_mode(void){ return nfsu2_test_run(); }

/* ---- the guard -------------------------------------------------------------- */
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
static void do_send(uint32_t ip, uint16_t port, const uint8_t *buf, int len, int is_bcast){
    if (!roam_plat_guard(g_cfg.test_mode, ip, is_bcast) || g_fd < 0) return;
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_len = sizeof sa; sa.sin_port = htons(port); sa.sin_addr.s_addr = htonl(ip);
    ssize_t n = sendto(g_fd, buf, (size_t)len, 0, (struct sockaddr *)&sa, sizeof sa);
    if (n == len){ g_st.sent++; g_st.sent_bytes += (uint64_t)len; return; }
    g_st.send_err++;
    if (g_st.send_err <= 5 || g_st.send_err % 1000 == 0){
        char b[16];
        int e = errno;
        fprintf(stderr, "[roam] sendto %s:%u failed: %s%s\n", roam_ip_str(ip, b), port, strerror(e),
                (e == EHOSTUNREACH || e == EPERM || e == ENETUNREACH) && !g_cfg.test_mode ?
                " (macOS: System Settings > Privacy & Security > Local Network must allow this app)" : "");
    }
}
static void io_send(void *u, uint32_t ip, uint16_t port, const uint8_t *buf, int len){ (void)u; do_send(ip, port, buf, len, 0); }

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
        if (!rc_addr_private(f->addr)) continue;            /* only private LANs */
        uint32_t b = (f->addr & f->mask) | ~f->mask;
        int dup = 0; for (int j = 0; j < k; j++) if (out[j] == b) dup = 1;
        if (!dup) out[k++] = b;
    }
    return k;
}
static void io_broadcast(void *u, uint16_t port, const uint8_t *buf, int len){
    (void)u;
    if (g_cfg.test_mode || !g_cfg.discovery){ roam_plat_guard(1, 0xFFFFFFFFu, 1); return; }
    roam_if ifs[32]; int n = 0;
    struct ifaddrs *ia = 0;
    if (getifaddrs(&ia) == 0){
        for (struct ifaddrs *p = ia; p && n < 32; p = p->ifa_next){
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || !p->ifa_netmask) continue;
            roam_if *f = &ifs[n++];
            snprintf(f->name, sizeof f->name, "%s", p->ifa_name);
            f->addr = ntohl(((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr);
            f->mask = ntohl(((struct sockaddr_in *)p->ifa_netmask)->sin_addr.s_addr);
            f->up = (p->ifa_flags & IFF_UP) && (p->ifa_flags & IFF_RUNNING);
            f->loopback = !!(p->ifa_flags & IFF_LOOPBACK);
            f->can_broadcast = !!(p->ifa_flags & IFF_BROADCAST);
        }
        freeifaddrs(ia);
    }
    uint32_t t[16]; int k = roam_bcast_targets(ifs, n, t, 16);
    for (int i = 0; i < k; i++) do_send(t[i], port, buf, len, 1);
}
static void io_event(void *u, int ev, int peer, const char *text);   /* roam_game.c provides the log */
void roam_game_event(int ev, int peer, const char *text);
static void io_event(void *u, int ev, int peer, const char *text){ (void)u; roam_game_event(ev, peer, text); }

/* ---- impairment (test aid) ------------------------------------------------------ */
static struct { int on, loss, delay, jitter, dup; uint32_t rng; } IMP;
#define IMPQ 512
static struct { int used; int64_t at; uint32_t ip; uint16_t port; int len; uint8_t b[RC_MAX_DGRAM]; } g_q[IMPQ];
static uint32_t imp_rand(void){ uint32_t x = IMP.rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; IMP.rng = x ? x : 1u; return IMP.rng; }
static void imp_parse(const char *s){
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
static void *net_main(void *u){
    (void)u;
    uint8_t buf[1500];
    int64_t next_tick = 0;
    while (!atomic_load(&g_stop)){
        struct pollfd pf = { g_fd, POLLIN, 0 };
        (void)poll(&pf, 1, 5);
        int64_t now = roam_plat_now_us();
        for (int k = 0; k < 256; k++){
            struct sockaddr_in sa; socklen_t sl = sizeof sa;
            ssize_t n = recvfrom(g_fd, buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&sa, &sl);
            if (n < 0) break;
            if (sa.sin_family != AF_INET) continue;
            uint32_t ip = ntohl(sa.sin_addr.s_addr); uint16_t port = ntohs(sa.sin_port);
            roam_plat_lock();
            g_st.recv++; g_st.recv_bytes += (uint64_t)n;
            if (n > RC_MAX_DGRAM){ g_s->rx++; g_s->rx_bad++; }
            else if (IMP.on){
                if ((int)(imp_rand() % 100u) < IMP.loss) g_st.impair_drop++;
                else imp_queue(now, ip, port, buf, (int)n);
            } else rc_receive(g_s, ip, port, buf, (int)n, now);
            roam_plat_unlock();
        }
        if (IMP.on){ roam_plat_lock(); imp_release(now); roam_plat_unlock(); }
        if (now >= next_tick){ roam_plat_lock(); rc_tick(g_s, now); roam_plat_unlock(); next_tick = now + 10000; }
    }
    return 0;
}

int roam_plat_start(const roam_plat_cfg *cfg, rc_session *s){
    g_cfg = *cfg; g_s = s;
    memset(&g_st, 0, sizeof g_st);
    if (g_cfg.test_mode && g_cfg.discovery){
        fprintf(stderr, "[roam] LAN discovery DISABLED: automated run (test harness), no broadcast will be sent; "
                        "only loopback peers from NFSU2_ROAM_PEERS\n");
        g_cfg.discovery = 0;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0){ fprintf(stderr, "[roam] socket: %s\n", strerror(errno)); return -1; }
    int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (g_cfg.discovery){ int one = 1; setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one); }
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_len = sizeof sa; sa.sin_port = htons(g_cfg.port);
    sa.sin_addr.s_addr = htonl(g_cfg.test_mode ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0){
        fprintf(stderr, "[roam] cannot use UDP port %u (%s): another program has it? Set the same other port "
                        "(roam_port / NFSU2_ROAM_PORT) on every machine\n", g_cfg.port, strerror(errno));
        close(fd); return -1;
    }
    g_fd = fd;
    imp_parse(g_cfg.impair);
    rc_io io = { 0, io_send, io_broadcast, io_event };
    roam_plat_lock(); s->io = io; roam_plat_unlock();
    atomic_store(&g_stop, 0);
    if (pthread_create(&g_thr, 0, net_main, 0) != 0){ close(fd); g_fd = -1; return -1; }
    g_thr_on = 1;
    fprintf(stderr, "[roam] UDP %s:%u open%s\n", g_cfg.test_mode ? "127.0.0.1" : "0.0.0.0", g_cfg.port,
            g_cfg.test_mode ? " (test harness: loopback only)" : g_cfg.discovery ? ", LAN discovery on" : "");
    return 0;
}
void roam_plat_stop(uint8_t reason){
    if (!g_thr_on) return;
    roam_plat_lock(); rc_shutdown(g_s, reason); roam_plat_unlock();
    atomic_store(&g_stop, 1);
    pthread_join(g_thr, 0); g_thr_on = 0;
    close(g_fd); g_fd = -1;
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
