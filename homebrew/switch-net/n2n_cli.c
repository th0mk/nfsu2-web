/* n2n_cli.c -- connects to a server, prints the welcome, the players and the round-trip time, then what comes in.
 *   n2n_cli ws://localhost:8080 [name] [seconds] [hub]
 * A smoke test of the client (test/run.sh runs it against a fake switch) and the model of how a game uses it:
 * one loop that polls, and sends between polls. */
#include "n2n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static n2n *g;

#ifdef N2N_TLS
void n2n_tls_set_ca(const char *pem);
/* wss://: the PEM bundle named by N2N_CA (e.g. /etc/ssl/certs/ca-certificates.crt) */
static void load_ca(void) {
    const char *path = getenv("N2N_CA");
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) { fprintf(stderr, "set N2N_CA to a PEM file of root certificates\n"); return; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) == (size_t)n) { b[n] = 0; n2n_tls_set_ca(b); }
    fclose(f);
}
#endif

static void ip(char *o, uint32_t v) { sprintf(o, "%u.%u.%u.%u", v >> 24, v >> 16 & 255, v >> 8 & 255, v & 255); }

static void on_event(void *user, const n2n_event *e) {
    char a[16];
    (void)user;
    switch (e->type) {
    case N2N_EV_WELCOME: ip(a, e->vip); printf("welcome: %s\n", a); n2n_ping(g); break;
    case N2N_EV_PEERS: {
        int n; const n2n_peer *p = n2n_peers(g, &n);
        printf("peers: %d\n", n);
        for (int i = 0; i < n; i++) { ip(a, p[i].vip); printf("  %s %s%s\n", a, p[i].name, p[i].server ? " (server)" : ""); }
        break;
    }
    case N2N_EV_PONG: printf("pong: %s\n", e->rtt_ms >= 0 ? "ok" : "?"); break;
    case N2N_EV_QUEUE: printf("in line: %d of %d\n", e->pos, e->of); break;
    case N2N_EV_FRAME: ip(a, e->src); printf("frame: kind %u from %s to port %u, %zu bytes\n", e->kind, a, e->dport, e->len); break;
    case N2N_EV_REMOVED: printf("%s: %s\n", e->ban ? "banned" : "kicked", e->msg); break;
    case N2N_EV_CLOSED: printf("closed: %d\n", e->code); break;
    }
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s ws://host:port [name] [seconds] [hub]\n", argv[0]); return 2; }
#ifdef N2N_TLS
    load_ca();
#endif
    int secs = argc > 3 ? atoi(argv[3]) : 5;
    g = n2n_new(argv[1], argc > 2 ? argv[2] : "n2n-cli", "n2n-cli-id", on_event, NULL);
    if (!g) { fprintf(stderr, "bad url\n"); return 2; }
    int r = n2n_connect(g, 5000);
    if (r) { fprintf(stderr, "connect failed: %d (http %d)\n", r, n2n_http_status(g)); return 1; }
    for (int i = 0; i < secs * 10; i++) {
        if (n2n_poll(g, 100) < 0) break;
        if (i == 5 && argc > 4 && n2n_online(g)) n2n_send_udp(g, N2N_ROAM_HUB, 1234, N2N_ROAM_PORT, "hi", 2);
    }
    n2n_free(g);
    return 0;
}
