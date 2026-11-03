/* lan_test.c -- lan_nx.c against fake-lan.mjs on a POSIX host: the two mailboxes as the game's runtime would fill them.
 *   cc -O1 -g -DNX_DIR='"/tmp/nxtest"' -DNET_IDLE_MS=1500 -Ihomebrew/switch-net runtime/switch/lan_nx.c homebrew/switch-net/n2n.c
 *      homebrew/switch-net/n2n_sock.c runtime/switch/tests/lan_test.c -o lan_test -lpthread
 *   node runtime/switch/tests/fake-lan.mjs 8098 & ./lan_test ws://127.0.0.1:8098 */
#include "../../web/net_proto.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void lan_nx_start(void); void lan_nx_stop(void); void lan_nx_report(FILE *f);
static net_mailbox *g_mb[2];
void *nfsu2_host_mailbox(int kind, uint32_t *size){ int i = kind == 4 ? 0 : kind == 7 ? 1 : -1; if (i < 0 || !g_mb[i]) return NULL; if (size) *size = NET_MAILBOX_BYTES; return g_mb[i]; }
int nfsu2_notify(int *p, unsigned n){ (void)p; (void)n; return 0; }
void nfsu2_pin(unsigned m){ (void)m; }
double nfsu2_now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }
static void sleep_ms(int ms){ struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 }; nanosleep(&ts, NULL); }

static net_mailbox *mbox(uint32_t mode){
    net_mailbox *m = aligned_alloc(64, NET_MAILBOX_BYTES); memset(m, 0, NET_MAILBOX_BYTES);
    m->magic = NET_MAGIC; m->version = NET_VERSION; m->hdr_bytes = NET_HDR_BYTES; m->slot_bytes = NET_SLOT_BYTES;
    m->tx_slots = NET_TX_SLOTS; m->rx_slots = NET_RX_SLOTS; m->tx_off = NET_HDR_BYTES; m->rx_off = NET_HDR_BYTES + NET_TX_SLOTS * NET_SLOT_BYTES;
    m->mode = mode; m->want = 1;
    return m;
}
static int fails;
#define CHECK(c, what) do { if (!(c)){ printf("FAIL: %s\n", what); fails++; } else printf("ok: %s\n", what); } while (0)
static void tx(net_mailbox *m, uint32_t kind, uint32_t dst, uint16_t sp, uint16_t dp, const void *data, uint32_t len){
    net_slot *s = (net_slot *)((uint8_t *)m + m->tx_off + (size_t)(m->tx_head % NET_TX_SLOTS) * NET_SLOT_BYTES);
    s->len = len; s->kind = kind; s->src_ip = 0; s->dst_ip = dst; s->src_port = sp; s->dst_port = dp; memcpy(s->data, data, len);
    atomic_store((_Atomic uint32_t *)&m->tx_head, m->tx_head + 1);
}
int main(int argc, char **argv){
    (void)argc;
    mkdir("/tmp/nxtest", 0777);
    FILE *f = fopen("/tmp/nxtest/online", "w"); if (f) fclose(f);
    f = fopen("/tmp/nxtest/server.txt", "w"); fprintf(f, "%s\n", argv[1]); fclose(f);
    remove("/tmp/nxtest/id.txt");
    g_mb[0] = mbox(NET_MODE_VLAN); g_mb[1] = mbox(NET_MODE_LAN);
    lan_nx_start();
    for (int i = 0; i < 100 && g_mb[0]->page_state != NET_PAGE_ONLINE; i++) sleep_ms(50);
    CHECK(g_mb[0]->page_state == NET_PAGE_ONLINE && g_mb[1]->page_state == NET_PAGE_ONLINE, "both mailboxes online");
    CHECK(g_mb[0]->vip == 0x0A4D0105u && g_mb[0]->netmask == 0xFFFFFF00u && g_mb[0]->generation == 1, "vip 10.77.1.5, netmask /24, generation 1");
    sleep_ms(200);                                    /* the peer list comes right after the welcome */
    /* peers: us (host 5) and the friend (host 6) and the server (host 1) in the bits */
    CHECK((g_mb[0]->peer_bits[0] & ((1u << 5) | (1u << 6) | (1u << 1))) == ((1u << 5) | (1u << 6)), "peer bits: hosts 5 and 6 of our /24 (the server, 10.77.0.1, is on another network)");
    CHECK(g_mb[0]->switch_peers == 2, "two humans on the switch (the server is not one)");
    sleep_ms(400);
    /* RX: the friend's roam datagram, the LAN broadcast */
    CHECK(g_mb[0]->rx_head == 1, "one frame in the roam RX ring");
    if (g_mb[0]->rx_head){
        net_slot *s = (net_slot *)((uint8_t *)g_mb[0] + g_mb[0]->rx_off);
        CHECK(s->kind == 1 && s->len == 4 && !memcmp(s->data, "ROAM", 4) && s->src_ip == 0x0A4D0106u && s->dst_port == 47210, "roam datagram's fields");
    }
    CHECK(g_mb[1]->rx_head == 1, "one frame in the LAN RX ring");
    if (g_mb[1]->rx_head){
        net_slot *s = (net_slot *)((uint8_t *)g_mb[1] + g_mb[1]->rx_off);
        CHECK(s->kind == 3 && s->len == 12 && !memcmp(s->data + 8, "LAN!", 4) && s->dst_ip == 0xFFFFFFFFu && s->dst_port == 3658, "LAN broadcast's fields");
    }
    /* TX: free roam to the hub, LAN broadcast, and a notice that must stay home */
    tx(g_mb[0], 1, 0x0A4D00FDu, 47210, 47210, "to-hub", 6);
    uint8_t lanp[8 + 7] = { 0, 64 }; memcpy(lanp + 8, "lan-out", 7);
    tx(g_mb[1], 3, 0xFFFFFFFFu, 3658, 3658, lanp, sizeof lanp);
    tx(g_mb[0], 16, 0, 0, 0, "Friend\x1f joined", 13);
    sleep_ms(500);
    CHECK(g_mb[0]->tx_tail == g_mb[0]->tx_head && g_mb[1]->tx_tail == g_mb[1]->tx_head, "the TX rings were drained");
    /* nothing wants the switch: it is given back after NET_IDLE_MS */
    g_mb[0]->want = 0; g_mb[1]->want = 0;
    for (int i = 0; i < 60 && g_mb[0]->page_state != NET_PAGE_IDLE; i++) sleep_ms(100);
    CHECK(g_mb[0]->page_state == NET_PAGE_IDLE, "idle after nothing wanted the switch");
    /* and wanted again: it connects again with a new generation */
    g_mb[0]->want = 1;
    for (int i = 0; i < 100 && !(g_mb[0]->page_state == NET_PAGE_ONLINE && g_mb[0]->generation == 2); i++) sleep_ms(100);
    printf("after the second want: state %u generation %u\n", g_mb[0]->page_state, g_mb[0]->generation);
    lan_nx_report(stdout);
    lan_nx_stop();
    sleep_ms(300);
    printf(fails ? "LAN TEST: %d FAILED\n" : "LAN TEST: OK (%d)\n", fails);
    return fails ? 1 : 0;
}
