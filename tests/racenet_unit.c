/* racenet_unit.c -- tests for runtime/racenet.c's car states straight between
 * the joiners' games ("N2CS") and the creator's list ("N2PL"), without the
 * game: who may send a joiner the list, and which slot each address may
 * speak for.
 *
 *   clang -O1 -g -Wall -Wno-unused-function -Iruntime -o out/racenet_unit tests/racenet_unit.c
 *   out/racenet_unit
 *
 * The game's memory is a 4 GB reservation of our own with only the pages a
 * test uses marked committed; the virtual LAN (net_lan.c) and the game's
 * handler of the creator's messages are stand-ins, the clock a fake one.
 * Cases, on a joiner (slot 1, the creator slot 0):
 *   stranger   a list from a race peer the game's race link does not send to
 *              (anyone who sends a datagram to our UDP 3658 is one): refused
 *   creator    the list from the creator (the race link sends to it): taken
 *   second     a list from another address the race link sends to: refused,
 *              the creator's kept
 *   direct     a car state from a joiner on the list: to the game's handler
 *   own-slot   that joiner naming another slot than its first: refused
 *   taken-slot another joiner naming that slot: refused; its own: taken
 *   rules      our slot, the creator's, a slot past 5, an address not on
 *              the list: refused
 *   reset      the race over: no list, no slots; the next race starts anew
 *   host-list  the creator: its list only to the race peers its race link
 *              sends to, naming only them (not a stranger who sent to 3658)
 * Every case prints "PASS <case> ..." or "FAIL <case> ..."; exit 1 on a FAIL. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

static uint64_t g_fake_ns = 5000000000u;
static int fake_clock_gettime(clockid_t id, struct timespec *ts){
    (void)id; ts->tv_sec = (time_t)(g_fake_ns / 1000000000u); ts->tv_nsec = (long)(g_fake_ns % 1000000000u); return 0;
}
#define clock_gettime(a, b) fake_clock_gettime((a), (b))
#include "../runtime/cpu.h"
uint8_t *g_mem;
uint8_t g_cbits[1u << 15];
static void commit(uint32_t a, uint32_t n){ for (uint32_t pg = a >> 14; pg <= (a + n - 1) >> 14; pg++) g_cbits[pg >> 3] |= (uint8_t)(1u << (pg & 7)); }

static int g_fail;
#define CHECK(cond, ...) do{ if (cond) printf("PASS " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); g_fail = 1; } \
                             printf("\n"); fflush(stdout); }while(0)

/* net_lan.c: the race's peers, the addresses the race link sends to, what was sent */
#define NPL_MAX_TEST 8
static uint32_t g_peer[4]; static int g_npeer;
static uint32_t g_sentto[4]; static int g_nsentto;
static int g_nout;
static uint32_t g_pl_to[8], g_pl_named[8][NPL_MAX_TEST]; static int g_pl_k[8], g_npl_out;   /* the creator's lists sent */
int vlan_rt_send(uint16_t port, uint32_t ip_h, const void *p, int n){
    (void)port; g_nout++;
    const uint8_t *m = p;
    if (n >= 8 && !memcmp(m, "N2PL", 4) && g_npl_out < 8){
        int k = g_npl_out++;
        g_pl_to[k] = ip_h; g_pl_k[k] = m[6];
        for (int i = 0; i < m[6] && i < NPL_MAX_TEST && 8 + 4 * i + 3 < n; i++)
            g_pl_named[k][i] = (uint32_t)m[8 + 4 * i] | (uint32_t)m[9 + 4 * i] << 8 | (uint32_t)m[10 + 4 * i] << 16 | (uint32_t)m[11 + 4 * i] << 24;
    }
    return 0;
}
int vlan_race_peers(uint32_t *ip_h, int max, uint32_t within_ms){ (void)within_ms; int n = g_npeer < max ? g_npeer : max; memcpy(ip_h, g_peer, 4u * (size_t)n); return n; }
int vlan_race_sent_to(uint32_t ip_h, uint32_t within_ms){ (void)within_ms; for (int i = 0; i < g_nsentto; i++) if (g_sentto[i] == ip_h) return 1; return 0; }
/* the lifted functions racenet.c wraps */
void sub_005f8d40_orig(CPU *c){ (void)c; }
void sub_005f8900_orig(CPU *c){ (void)c; }
void sub_00585d50_orig(CPU *c){ (void)c; }
/* the game's handler of the creator's messages, as dispatch_indirect calls it */
static int g_calls; static uint32_t g_fn, g_len; static uint8_t g_bytes[64];
void dispatch_indirect(CPU *c, uint32_t addr);
#include "../runtime/racenet.c"
void dispatch_indirect(CPU *c, uint32_t addr){
    g_calls++; g_fn = addr;
    uint32_t buf = MR32(c->esp + 4u); g_len = MR32(c->esp + 8u);
    for (uint32_t i = 0; i < g_len && i < sizeof g_bytes; i++) g_bytes[i] = MR8(buf + i);
    c->esp += 4u;                                        /* its ret */
}

/* a network race on a joiner: a world with a friend's car and ours */
#define W      0x10000000u
#define CAR_A  0x10010000u
#define CAR_ME 0x10020000u
#define PL     0x10030000u
#define RC     0x10040000u
#define STACK  0x00130000u
static CPU g_c;
static void race_setup(void){
    uint32_t g[] = { G_FLOW, G_NETMODE, G_NET_HOST, G_WORLD, G_PLAYER1, G_WORLDDT, G_RACE, G_WORLDTIME, G_NETPLAYERS,
                     G_MYSLOT, G_HOSTSLOT, G_SLOTBITS, G_SLOTMIN };
    for (unsigned i = 0; i < sizeof g / sizeof g[0]; i++) commit(g[i], 32);
    commit(W, 0x100); commit(CAR_A, 0xa00); commit(CAR_ME, 0xa00); commit(PL, 16); commit(RC, 0xc00);
    commit(STACK - 0x4000u, 0x8000u);
    MR32(G_FLOW) = 6; MR32(G_NETMODE) = 8; MR32(G_NET_HOST) = 0;
    MR32(G_WORLD) = W; MR32(W) = CAR_A; MR32(CAR_A) = CAR_ME; MR32(CAR_ME) = W;
    MR32(G_PLAYER1) = PL; MR32(PL + 4u) = CAR_ME;
    MR32(G_RACE) = RC; MR32(RC) = RC_RACING; MR32(RC + 0x24u) = 0;
    MRF32(G_WORLDDT) = 0.02f;
    MR32(G_SLOTBITS) = 3; MR32(G_SLOTMIN) = 0; MR32(G_MYSLOT) = 1; MR32(G_HOSTSLOT) = 0;
    MR16(CAR_A + 0xau) = 0xffffu; MR16(CAR_ME + 0xau) = 0xffffu;
    g_c.esp = STACK;
}
static void frame(void){ racenet_frame(&g_c); g_fake_ns += 20000000u; MR32(G_WORLDTIME) += 80u; }
/* a car state message: kind 2, then at bit 40 the slot in 3 bits */
static int carspam(uint8_t *m, uint32_t slot, int len){
    memset(m, 0, CS_HDR + (size_t)len);
    memcpy(m, "N2CS", 4); m[4] = 1; m[6] = (uint8_t)len; m[7] = (uint8_t)(len >> 8);
    m[CS_HDR] = MSG_CARSPAM;
    m[CS_HDR + 5] = (uint8_t)(slot << 5);
    for (int i = 6; i < len; i++) m[CS_HDR + i] = (uint8_t)(0x40 + i);
    return (int)CS_HDR + len;
}
/* the creator's list: the given addresses */
static int list(uint8_t *m, const uint32_t *ip, int k){
    memcpy(m, "N2PL", 4); m[4] = 1; m[5] = 0; m[6] = (uint8_t)k; m[7] = 0;
    for (int i = 0; i < k; i++) for (int b = 0; b < 4; b++) m[8 + 4 * i + b] = (uint8_t)(ip[i] >> (8 * b));
    return 8 + 4 * k;
}

int main(void){
    g_mem = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    if (g_mem == MAP_FAILED){ printf("no 4 GB reservation\n"); return 1; }
    const uint32_t creator = 0x0a4d0101u, j2 = 0x0a4d0303u, j3 = 0x0a4d0404u, stranger = 0x0a4d0909u;
    race_setup();
    frame();                                             /* the race begins */

    uint8_t pl[8 + 4 * NPL], m[CS_HDR + 64];
    const uint32_t joiners[2] = { j2, j3 }, mine[1] = { stranger };
    int n = list(pl, joiners, 2);
    g_peer[0] = creator; g_peer[1] = stranger; g_npeer = 2;   /* a datagram to our UDP 3658 makes anyone a race peer */
    g_sentto[0] = creator; g_nsentto = 1;                     /* the game's race link sends only to its creator */
    racenet_list(pl, n, stranger);
    CHECK(g_pl.n == 0 && R.pl_refused == 1, "stranger: a list from a race peer the game does not send to: refused (%u, %u)", g_pl.n, R.pl_refused);

    racenet_list(pl, n, creator);
    CHECK(g_pl.n == 2 && g_pl.ip[0] == j2 && g_pl.ip[1] == j3 && g_pl.from == creator, "creator: its list taken (%u)", g_pl.n);

    g_sentto[1] = stranger; g_nsentto = 2;
    n = list(pl, mine, 1);
    racenet_list(pl, n, stranger);
    CHECK(g_pl.n == 2 && g_pl.from == creator && g_pl.ip[0] == j2 && R.pl_refused == 2,
          "second: a list from another address the race link sends to: refused, the creator's kept (%u)", R.pl_refused);
    g_nsentto = 1;

    n = carspam(m, 3, 20);
    racenet_carspam(&g_c, m, n, j2);
    CHECK(g_calls == 1 && g_fn == FN_JOIN_MSG && g_len == 20 && !memcmp(g_bytes, m + CS_HDR, 20) && R.cs_came == 1,
          "direct: j2's state for slot 3 to the game's handler (%d calls)", g_calls);
    racenet_carspam(&g_c, m, n, j2);
    CHECK(g_calls == 2, "direct: j2's next state for slot 3 too (%d calls)", g_calls);

    n = carspam(m, 2, 20); racenet_carspam(&g_c, m, n, j2);
    CHECK(g_calls == 2 && R.cs_refused == 1, "own-slot: j2 naming slot 2 after slot 3: refused (%u)", R.cs_refused);

    n = carspam(m, 3, 20); racenet_carspam(&g_c, m, n, j3);
    CHECK(g_calls == 2 && R.cs_refused == 2, "taken-slot: j3 naming j2's slot 3: refused (%u)", R.cs_refused);
    n = carspam(m, 2, 20); racenet_carspam(&g_c, m, n, j3);
    CHECK(g_calls == 3 && R.cs_came == 3, "taken-slot: j3 naming slot 2, its first: taken (%d calls)", g_calls);
    n = carspam(m, 3, 20); racenet_carspam(&g_c, m, n, j3);
    CHECK(g_calls == 3 && R.cs_refused == 3, "taken-slot: j3 then naming slot 3: refused (%u)", R.cs_refused);

    n = carspam(m, 1, 20); racenet_carspam(&g_c, m, n, j2);
    int ok1 = g_calls == 3 && R.cs_refused == 4;
    n = carspam(m, 0, 20); racenet_carspam(&g_c, m, n, j2);
    int ok0 = g_calls == 3 && R.cs_refused == 5;
    n = carspam(m, 7, 20); racenet_carspam(&g_c, m, n, j2);
    int ok7 = g_calls == 3 && R.cs_refused == 6;
    n = carspam(m, 4, 20); racenet_carspam(&g_c, m, n, stranger);
    int oks = g_calls == 3 && R.cs_refused == 7;
    CHECK(ok1 && ok0 && ok7 && oks, "rules: our slot %d, the creator's %d, past 5 %d, not on the list %d: refused", ok1, ok0, ok7, oks);

    MR32(RC) = 0; frame();                               /* the race over */
    CHECK(g_pl.n == 0 && g_pl.from == 0 && g_nbind == 0, "reset: the race over: no list, no slots (%u %d)", g_pl.n, g_nbind);
    n = carspam(m, 3, 20); racenet_carspam(&g_c, m, n, j2);
    int out = g_calls == 3;
    MR32(RC) = RC_RACING; frame();                       /* the next race: j3 now has slot 3 */
    n = list(pl, joiners, 2); racenet_list(pl, n, creator);
    n = carspam(m, 3, 20); racenet_carspam(&g_c, m, n, j3);
    CHECK(out && g_calls == 4 && g_nbind == 1 && g_bind[0].ip == j3 && g_bind[0].slot == 3,
          "reset: out of the race nothing taken; in the next one j3 may name slot 3 (%d calls)", g_calls);

    /* the creator: a stranger who sent a datagram to its UDP 3658 is a race
     * peer too, but its race link sends only to its joiners */
    MR32(G_NET_HOST) = 1;
    g_peer[0] = j2; g_peer[1] = stranger; g_peer[2] = j3; g_npeer = 3;
    g_sentto[0] = j2; g_sentto[1] = j3; g_nsentto = 2;
    g_npl_out = 0; frame();
    int to_s = 0, named_s = 0, each_other = g_npl_out == 2;
    for (int k = 0; k < g_npl_out; k++){
        to_s |= g_pl_to[k] == stranger;
        for (int i = 0; i < g_pl_k[k] && i < NPL_MAX_TEST; i++) named_s |= g_pl_named[k][i] == stranger;
        each_other &= g_pl_k[k] == 1 && g_pl_named[k][0] == (g_pl_to[k] == j2 ? j3 : j2);
    }
    CHECK(each_other && !to_s && !named_s, "host-list: the list only to the joiners the race link sends to, naming only them "
          "(%d lists, to the stranger %d, naming it %d)", g_npl_out, to_s, named_s);

    printf(g_fail ? "racenet unit tests: FAILED\n" : "racenet unit tests: all passed\n");
    return g_fail;
}
