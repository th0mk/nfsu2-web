/* roam_probe.c -- a free-roam player that is not a game: the real core (roam_core.c) over n2n to the server's hub, standing at
 * one spot, printing what the hub gives it.  Tells whether the hub lists players for a plain client (the way the console's
 * game gets none).
 *   cc -O1 -g -Iruntime -Ihomebrew/switch-net -DN2N_TLS ... runtime/switch/tests/roam_probe.c runtime/roam_core.c
 *      homebrew/switch-net/n2n.c homebrew/switch-net/n2n_sock.c -o roam_probe -lpthread -lm
 *   roam_probe <wss url> <name> <x> <y> <z> <seconds> [links] */
#include "roam_core.h"
#include "n2n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

extern const char nfsu2_cacert_pem[];
void n2n_tls_set_ca(const char *pem);

static n2n *g_c; static uint32_t g_vip; static int g_welcome;
static rc_session S;
static double now_us(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }

static void io_send(void *u, uint32_t ip, uint16_t port, const uint8_t *b, int n){
    (void)u; if (!g_welcome || n > 1400) return;
    uint8_t f[16 + 1400];
    f[0] = 1; f[1] = f[2] = f[3] = 0;
    f[4] = g_vip >> 24; f[5] = g_vip >> 16; f[6] = g_vip >> 8; f[7] = g_vip;
    f[8] = ip >> 24; f[9] = ip >> 16; f[10] = ip >> 8; f[11] = ip;
    f[12] = 47210 >> 8; f[13] = 47210 & 255; f[14] = port >> 8; f[15] = port & 255;
    memcpy(f + 16, b, n);
    n2n_send_frame(g_c, f, 16 + n);
}
static void io_event(void *u, int ev, int peer, const char *t){ (void)u; (void)peer; printf("  event %d: %s\n", ev, t ? t : ""); }
static void on_event(void *user, const n2n_event *ev){
    (void)user;
    if (ev->type == N2N_EV_WELCOME){ g_vip = ev->vip; g_welcome = 1; printf("welcome %u.%u.%u.%u\n", ev->vip >> 24, ev->vip >> 16 & 255, ev->vip >> 8 & 255, ev->vip & 255); }
    else if (ev->type == N2N_EV_FRAME && ev->kind == 1) rc_receive(&S, ev->src, ev->sport, ev->data, (int)ev->len, (int64_t)now_us());
    else if (ev->type == N2N_EV_CLOSED) printf("closed %d\n", ev->code);
}

int main(int argc, char **argv){
    if (argc < 7){ fprintf(stderr, "usage: roam_probe url name x y z seconds [links]\n"); return 2; }
    float x = (float)atof(argv[3]), y = (float)atof(argv[4]), z = (float)atof(argv[5]); int secs = atoi(argv[6]);
    int links = argc > 7 ? atoi(argv[7]) : 16;
    n2n_tls_set_ca(nfsu2_cacert_pem);
    char id[33]; for (int i = 0; i < 16; i++) snprintf(id + 2 * i, 3, "%02x", rand() & 255);
    g_c = n2n_new(argv[1], argv[2], id, on_event, NULL);
    n2n_set_lan(g_c, 1, 0);
    if (n2n_connect(g_c, 10000)){ printf("connect failed\n"); return 1; }
    for (int i = 0; i < 100 && !g_welcome; i++) n2n_poll(g_c, 50);
    if (!g_welcome){ printf("no welcome\n"); return 1; }
    rc_config cf; memset(&cf, 0, sizeof cf);
    cf.self_id = (uint32_t)rand() | 1; cf.port = 47210; cf.room = 0; cf.platform = RC_PLAT_MAC;
    cf.max_players = RC_MAX_PLAYERS; cf.rate_hz = 20; cf.snap_states = 1;
    cf.hub_ip = 0x0A4D00FDu; cf.hub_port = 47210; cf.links = links;
    snprintf(cf.name, sizeof cf.name, "%s", argv[2]);
    rc_io io = { 0, io_send, 0, io_event };
    rc_session_init(&S, &cf, &io);
    rc_carcfg cc; memset(&cc, 0, sizeof cc);
    cc.type_hash = 0x43DB50u; cc.ride_b[3] = 1; snprintf(cc.type_name, RC_CFG_TYPE_NAME, "350Z");
    uint8_t rec[RC_CFG_MAX]; int len = rc_carcfg_encode(&cc, rec, sizeof rec);
    if (len > 0) rc_set_cfg(&S, rec, len, (int64_t)now_us());
    rc_set_status(&S, RC_ST_ROAM);
    double t0 = now_us(), next_print = t0 + 5e6, next_state = 0;
    while (now_us() - t0 < secs * 1e6){
        n2n_poll(g_c, 10);
        double t = now_us();
        if (t >= next_state){
            rc_state st; memset(&st, 0, sizeof st);
            st.t_us = (uint32_t)(int64_t)t; st.p[0] = x; st.p[1] = y; st.p[2] = z; st.q[3] = 1.f; st.rpm = 1000.f; st.gear = 1;
            rc_push_state(&S, &st, (int64_t)t); next_state = t + 50000;
        }
        rc_tick(&S, (int64_t)t);
        if (t >= next_print){
            next_print += 5e6;
            printf("t=%.0fs lists %u (last %u, longest %u) established %d\n", (t - t0) / 1e6, S.hub_lists, S.hub_list_last, S.hub_list_max, rc_established(&S));
            fflush(stdout);
        }
    }
    rc_shutdown(&S, 0);
    n2n_close(g_c); n2n_free(g_c);
    return 0;
}
