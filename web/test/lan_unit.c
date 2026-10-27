/* lan_unit.c -- LAN play's Winsock HLE in the browser, without the game:
 * runtime/winsock.c (the front end with Windows semantics) over
 * runtime/web/net_lan.c (the virtual-LAN backend), built for wasm with the
 * game's flags and run under node (web/test/lan-wasm.test.mjs).
 *
 * The test plays both the page and the other browsers on the switch: it
 * writes our address into the LAN mailbox, reads what the backend queued on
 * the TX ring (frames to the switch) and puts frames from "10.77.0.3" on the
 * RX ring, then pumps the backend (NFSU2_LAN_NOTHREAD=1: no LAN thread).  The
 * calls are made the way runtime/win32.c makes them (a CPU whose esp points at
 * the stdcall arguments, a TEB for the last error).  Checks follow the Mac's
 * tests/net_harness.c and tools/vlanhub.py's rules: bind semantics, UDP
 * broadcast with the own copy, port unreachable, WSAEMSGSIZE, TCP in this
 * browser (connect/accept/send/recv, FIN, RST, refused) and to another
 * browser (SYN/SYNACK/DATA/FIN/RST, GONE, link loss), ICMP echo, select,
 * Netbios, identity; a port-unreachable notice reaching the socket that sent
 * (several binders of one port), control frames (FIN) after a send that
 * filled the TX ring, the answers the backend gives by itself (port
 * unreachable, echo reply, RST) limited per sender and in all, and the page
 * closing its link at exit (the log line, checked by the .mjs).  Also SHA-256 (runtime/web/sha256.c) against FIPS
 * 180-4 vectors.  Output: one line per check, then "lan_unit: N passed, M failed".
 * `lan_unit pin <file>` only hashes a game DLL as the browser's loader does (runtime/module.c's pin), and the same
 * bytes with the middle one changed: "lan_unit: pin N bytes, sha256 <hex>, one byte changed <hex>". */
#include "cpu.h"
#include "game.h"
#include "winsock.h"
#include "net_proto.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int winsock_call(CPU *c, const char *dll, const char *n, uint32_t *out);
net_mailbox *vlan_test_mailbox(void);
int vlan_test_pump(void);
void nfsu2_sha256(const void *data, size_t n, uint8_t out[32]);
void mem_commit(uint32_t, uint32_t);

/* the game winsock.c serves: UG2's Online host (no exe image here, so
 * winsock.c leaves it alone) */
const GameOps g_game = { .id = "ug2", .exe_name = "SPEED2.EXE", .vroot = "C:\\NFSU2",
                         .online_host_va = 0x007fbdbcu, .online_host = "pcnfs05.ea.com" };
void win32_set_last_error(CPU *c, uint32_t e){ if (c && c->fs_base) MR32(c->fs_base + 0x34u) = e; }
uint32_t win32_get_last_error(CPU *c){ return c && c->fs_base ? MR32(c->fs_base + 0x34u) : 0u; }
void guest_wait_begin(void){ }
void guest_wait_end(void){ }
void online_lobby_read(uint32_t link, const uint8_t *p, int n){ (void)link; (void)p; (void)n; }   /* online.c */
void olfilter_lobby_mark(uint32_t link, uint8_t *p, int n){ (void)link; (void)p; (void)n; }       /* olfilter.c */
void olfilter_lobby_sent(uint32_t link, uint8_t *p, int n){ (void)link; (void)p; (void)n; }

static int g_pass, g_fail;
static void check(const char *what, int ok, const char *fmt, ...){
    char b[300] = "";
    if (fmt){ va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); }
    printf("  %s  %-62s %s\n", ok ? "PASS" : "FAIL", what, b);
    if (ok) g_pass++; else g_fail++;
}

/* ---- guest memory: plain linear memory in the test (g_mem == 0 on wasm) ---- */
static uint8_t g_teb[0x1000] __attribute__((aligned(16)));
static uint32_t g_args[16];
static uint8_t g_buf[8192], g_buf2[8192], g_sa[16], g_sa2[16], g_len[4], g_wsa[400], g_set[3][4 + 4 * 64], g_tv[8], g_opt[8], g_ncb[64], g_ast[64];
static CPU g_cpu;
#define GA(p) ((uint32_t)(uintptr_t)(p))

static uint32_t call(const char *dll, const char *n, int nargs, ...){
    va_list ap; va_start(ap, nargs);
    for (int i = 0; i < nargs; i++) g_args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    g_cpu.esp = GA(g_args);
    uint32_t r = 0xDEADBEEFu;
    if (!winsock_call(&g_cpu, dll, n, &r)) printf("  (not handled: %s)\n", n);
    return r;
}
#define WS(n, k, ...) call("ws2_32.dll", n, k, __VA_ARGS__)
static uint32_t err(void){ return MR32(GA(g_teb) + 0x34u); }
static uint32_t ipn(int a, int b, int c, int d){ return (uint32_t)a | (uint32_t)b << 8 | (uint32_t)c << 16 | (uint32_t)d << 24; }   /* network order */
static uint32_t sa(uint8_t *p, uint32_t ip, uint16_t port){
    memset(p, 0, 16); p[0] = 2; p[2] = (uint8_t)(port >> 8); p[3] = (uint8_t)port; memcpy(p + 4, &ip, 4); return GA(p); }
static uint16_t sa_port(const uint8_t *p){ return (uint16_t)(p[2] << 8 | p[3]); }
static uint32_t sa_ip(const uint8_t *p){ uint32_t v; memcpy(&v, p + 4, 4); return v; }
static uint32_t lenp(uint32_t v){ memcpy(g_len, &v, 4); return GA(g_len); }
static uint32_t setof(int k, int n, ...){
    va_list ap; va_start(ap, n);
    uint32_t c = (uint32_t)n; memcpy(g_set[k], &c, 4);
    for (int i = 0; i < n; i++){ uint32_t h = va_arg(ap, uint32_t); memcpy(g_set[k] + 4 + 4 * i, &h, 4); }
    va_end(ap); return GA(g_set[k]);
}
static uint32_t setn(int k){ uint32_t c; memcpy(&c, g_set[k], 4); return c; }
static uint32_t tv0(void){ memset(g_tv, 0, 8); return GA(g_tv); }
static uint32_t one(uint32_t v){ memcpy(g_opt, &v, 4); return GA(g_opt); }
static void nb(uint32_t s){ WS("ioctlsocket", 3, s, 0x8004667Eu, one(1)); }

/* ---- the page and the other browsers ---- */
static net_mailbox *MB;
#define ME   0x0A4D0002u                 /* host order: 10.77.0.2 */
#define PEER 0x0A4D0003u                 /* 10.77.0.3, on the switch */
typedef struct { uint32_t kind, src, dst, len; uint16_t sport, dport; uint8_t op; uint32_t cid; uint8_t d[1536]; int n; } Frame;
static net_slot *slot(uint32_t off, uint32_t slots, uint32_t i){ return (net_slot *)((uint8_t *)MB + off + (i % slots) * NET_SLOT_BYTES); }
static int tx_take(Frame *f){
    if (MB->tx_tail == MB->tx_head) return 0;
    net_slot *s = slot(MB->tx_off, NET_TX_SLOTS, MB->tx_tail);
    f->kind = s->kind; f->src = s->src_ip; f->dst = s->dst_ip; f->sport = s->src_port; f->dport = s->dst_port; f->len = s->len;
    f->op = s->data[0]; memcpy(&f->cid, s->data + 4, 4);
    f->n = (int)s->len - (int)NET_LAN_HDR; if (f->n > 0) memcpy(f->d, s->data + NET_LAN_HDR, (size_t)f->n);
    MB->tx_tail++;
    return 1;
}
static int tx_count(void){ return (int)(MB->tx_head - MB->tx_tail); }
static void tx_drain(void){ Frame f; while (tx_take(&f)) {} }
static void rx_put(uint32_t kind, uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport, uint8_t op, uint32_t cid, const void *d, int n){
    net_slot *s = slot(MB->rx_off, NET_RX_SLOTS, MB->rx_head);
    s->len = NET_LAN_HDR + (uint32_t)n; s->kind = kind; s->src_ip = src; s->dst_ip = dst; s->src_port = sport; s->dst_port = dport;
    memset(s->data, 0, NET_LAN_HDR); s->data[0] = op; memcpy(s->data + 4, &cid, 4);
    if (n) memcpy(s->data + NET_LAN_HDR, d, (size_t)n);
    MB->rx_head++;
    vlan_test_pump();
}

static void sha_tests(void){
    static const struct { const char *in; int reps; const char *hex; } V[] = {
        { "abc", 1, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "", 1, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 1, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
        { "a", 1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0" },
    };
    for (int k = 0; k < 4; k++){
        size_t n = strlen(V[k].in) * (size_t)V[k].reps; char *b = malloc(n + 1);
        for (int r = 0; r < V[k].reps; r++) memcpy(b + strlen(V[k].in) * (size_t)r, V[k].in, strlen(V[k].in));
        uint8_t d[32]; nfsu2_sha256(b, n, d); free(b);
        char hex[65]; for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
        check("sha256 FIPS 180-4 vector", !strcmp(hex, V[k].hex), "%.16s... (%zu bytes)", hex, n);
    }
}

/* the pin of a lifted DLL: runtime/module.c maps the file only when this hash is the one its ModInfo records */
static int pin_hash(const char *path){
    FILE *f = fopen(path, "rb");
    if (!f){ printf("lan_unit: pin: cannot read %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 ? malloc((size_t)n) : 0;
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n){ fclose(f); free(b); printf("lan_unit: pin: cannot read %s\n", path); return 1; }
    fclose(f);
    char hex[2][65];
    for (int k = 0; k < 2; k++){
        uint8_t d[32]; nfsu2_sha256(b, (size_t)n, d);
        for (int i = 0; i < 32; i++) snprintf(hex[k] + 2 * i, 3, "%02x", d[i]);
        b[n / 2] ^= 1;                                       /* then one byte changed */
    }
    free(b);
    printf("lan_unit: pin %ld bytes, sha256 %s, one byte changed %s\n", n, hex[0], hex[1]);
    return 0;
}

int main(int argc, char **argv){
    if (argc == 3 && !strcmp(argv[1], "pin")) return pin_hash(argv[2]);
    setenv("NFSU2_LAN_NOTHREAD", "1", 1);
    mem_commit(0x1000, 0x00F00000u);                         /* the test's whole low memory is "guest memory" */
    g_cpu.fs_base = GA(g_teb); MR32(GA(g_teb) + 0x18) = GA(g_teb); MR32(GA(g_teb) + 0x24) = 1;
    sha_tests();

    printf("identity and startup\n");
    MB = vlan_test_mailbox();
    check("LAN mailbox registered", MB && MB->magic == NET_MAGIC && MB->mode == NET_MODE_LAN, 0);
    MB->vip = ME; MB->netmask = 0xFFFFFF00u; MB->generation = 1;
    MB->peer_bits[0] = (1u << 2) | (1u << 3); MB->page_state = NET_PAGE_ONLINE;
    uint32_t r = WS("WSAStartup", 2, 0x0002u, GA(g_wsa));
    check("WSAStartup(2.0) on the virtual LAN", r == 0 && (g_wsa[0] | g_wsa[1] << 8) == 0x0002, "r=%u", r);
    check("a network session: the mailbox wants the switch", MB->want == 1, "want %u", MB->want);
    r = WS("gethostname", 2, GA(g_buf), 64u);
    check("gethostname = VLAN-2", r == 0 && !strcmp((char *)g_buf, "VLAN-2"), "\"%s\"", g_buf);
    { uint32_t s = WS("socket", 3, 2u, 2u, 0u);
      memset(g_buf2, 0, 32);
      r = WS("WSAIoctl", 9, s, 0xC8000014u, sa(g_sa, ipn(24, 1, 1, 1), 0), 16u, GA(g_buf2), 16u, lenp(0), 0u, 0u);
      check("route query toward 24.1.1.1 = our address", r == 0 && sa_ip(g_buf2) == ipn(10, 77, 0, 2), 0);
      WS("closesocket", 1, s); }
    memset(g_ncb, 0, 64); memset(g_ast, 0, 64);
    g_ncb[0] = 0x33; memcpy(g_ncb + 4, &(uint32_t){GA(g_ast)}, 4); g_ncb[8] = 60; g_ncb[10] = '*';
    r = call("netapi32.dll", "Netbios", 1, GA(g_ncb));
    check("Netbios NCBASTAT: MAC 02:4e:56:4c:00:02", r == 0 && !memcmp(g_ast, "\x02\x4e\x56\x4c\x00\x02", 6), "rc %u", r);

    printf("UDP\n");
    uint32_t u1 = WS("socket", 3, 2u, 2u, 0u), u2 = WS("socket", 3, 2u, 2u, 0u), u3 = WS("socket", 3, 2u, 2u, 0u);
    nb(u1); nb(u2); nb(u3);
    check("socket(AF_INET, SOCK_DGRAM)", u1 != 0xFFFFFFFFu && u2 != 0xFFFFFFFFu && u1 != 0, "0x%x", u1);
    r = WS("bind", 3, u1, sa(g_sa, 0, 9999), 16u);
    check("bind 0.0.0.0:9999", r == 0, "err %u", err());
    r = WS("bind", 3, u2, sa(g_sa, 0, 9999), 16u);
    check("second bind of 9999 without SO_REUSEADDR: WSAEADDRINUSE", r == 0xFFFFFFFFu && err() == WSAEADDRINUSE, "err %u", err());
    WS("setsockopt", 5, u2, 0xFFFFu, 4u, one(1), 4u);
    r = WS("bind", 3, u2, sa(g_sa, 0, 9999), 16u);
    check("... with SO_REUSEADDR: bound", r == 0, "err %u", err());
    r = WS("bind", 3, u3, sa(g_sa, ipn(10, 77, 0, 9), 0), 16u);
    check("bind to an address that is not ours: WSAEADDRNOTAVAIL", r == 0xFFFFFFFFu && err() == WSAEADDRNOTAVAIL, "err %u", err());
    r = WS("bind", 3, u3, sa(g_sa, 0, 3658), 16u);
    check("bind 0.0.0.0:3658", r == 0, 0);
    memcpy(g_buf, "gEA?query", 9);
    r = WS("sendto", 6, u1, GA(g_buf), 9u, 0u, sa(g_sa, 0xFFFFFFFFu, 9999), 16u);
    check("broadcast without SO_BROADCAST: WSAEACCES", r == 0xFFFFFFFFu && err() == WSAEACCES, "err %u", err());
    WS("setsockopt", 5, u1, 0xFFFFu, 0x20u, one(1), 4u);
    tx_drain();
    r = WS("sendto", 6, u1, GA(g_buf), 9u, 0u, sa(g_sa, 0xFFFFFFFFu, 9999), 16u);
    Frame f; int got = tx_take(&f);
    check("broadcast goes to the switch's broadcast address", r == 9 && got && f.kind == NET_KIND_LAN_UDP && f.dst == NET_BROADCAST &&
          f.sport == 9999 && f.dport == 9999 && f.n == 9 && !memcmp(f.d, "gEA?query", 9), "r=%d kind %u dst %08x", (int)r, f.kind, f.dst);
    memset(g_buf2, 0, 64);
    r = WS("recvfrom", 6, u1, GA(g_buf2), 100u, 0u, GA(g_sa2), lenp(16));
    check("own broadcast looped back to the sender (Windows)", r == 9 && sa_ip(g_sa2) == ipn(10, 77, 0, 2) && sa_port(g_sa2) == 9999, "r=%d", (int)r);
    r = WS("recvfrom", 6, u2, GA(g_buf2), 100u, 0u, GA(g_sa2), lenp(16));
    check("... and to the second binder of the port", r == 9, "r=%d", (int)r);
    r = WS("recvfrom", 6, u1, GA(g_buf2), 100u, 0u, GA(g_sa2), lenp(16));
    check("then empty: WSAEWOULDBLOCK", r == 0xFFFFFFFFu && err() == WSAEWOULDBLOCK, "err %u", err());
    rx_put(NET_KIND_LAN_UDP, PEER, NET_BROADCAST, 9999, 9999, 0, 0, "gEA!advert", 10);
    r = WS("recvfrom", 6, u1, GA(g_buf2), 100u, 0u, GA(g_sa2), lenp(16));
    check("a broadcast from 10.77.0.3 arrives with its source", r == 10 && sa_ip(g_sa2) == ipn(10, 77, 0, 3) && sa_port(g_sa2) == 9999 &&
          !memcmp(g_buf2, "gEA!advert", 10), "r=%d", (int)r);
    tx_drain();
    r = WS("sendto", 6, u3, GA(g_buf), 9u, 0u, sa(g_sa, ipn(10, 77, 0, 3), 3658), 16u);
    got = tx_take(&f);
    check("unicast to 10.77.0.3:3658", r == 9 && got && f.dst == PEER && f.dport == 3658 && f.sport == 3658, 0);
    rx_put(NET_KIND_LAN_UDP, PEER, ME, 3658, 3658, 0, 0, "0123456789ABCDEF", 16);
    memset(g_buf2, 0, 64);
    r = WS("recvfrom", 6, u3, GA(g_buf2), 8u, 0u, GA(g_sa2), lenp(16));
    check("an 8-byte buffer: WSAEMSGSIZE with the head copied", r == 0xFFFFFFFFu && err() == WSAEMSGSIZE && !memcmp(g_buf2, "01234567", 8), "err %u", err());
    r = WS("recvfrom", 6, u3, GA(g_buf2), 100u, 0u, GA(g_sa2), lenp(16));
    check("... and the rest discarded", r == 0xFFFFFFFFu && err() == WSAEWOULDBLOCK, "err %u", err());
    tx_drain();
    rx_put(NET_KIND_LAN_UDP, PEER, ME, 5000, 4444, 0, 0, "x", 1);
    got = tx_take(&f);
    check("a datagram to a port nobody holds: port unreachable back", got && f.op == NET_LAN_UDP_UNREACH && f.dst == PEER && f.dport == 5000 && f.sport == 4444, 0);
    WS("connect", 3, u3, sa(g_sa, ipn(10, 77, 0, 3), 3658), 16u);
    rx_put(NET_KIND_LAN_UDP, PEER, ME, 3658, 3658, NET_LAN_UDP_UNREACH, 0, 0, 0);
    r = WS("recv", 4, u3, GA(g_buf2), 100u, 0u);
    check("connected UDP after a port-unreachable notice: WSAECONNRESET", r == 0xFFFFFFFFu && err() == WSAECONNRESET, "err %u", err());
    tx_drain();
    r = WS("sendto", 6, u1, GA(g_buf), 9u, 0u, sa(g_sa, ipn(159, 153, 253, 82), 9999), 16u);
    check("outside the virtual LAN: sent and dropped (nothing on the switch)", r == 9 && tx_count() == 0, 0);
    r = WS("select", 5, 0u, setof(0, 2, u1, u3), setof(1, 1, u1), 0u, tv0());
    check("select: idle UDP sockets are writable only", r == 1 && setn(0) == 0 && setn(1) == 1, "r=%d", (int)r);
    rx_put(NET_KIND_LAN_UDP, PEER, ME, 3658, 3658, 0, 0, "abc", 3);
    r = WS("select", 5, 0u, setof(0, 2, u1, u3), 0u, 0u, tv0());
    check("select: a datagram makes it readable", r == 1 && setn(0) == 1, "r=%d", (int)r);
    WS("recv", 4, u3, GA(g_buf2), 100u, 0u);
    WS("closesocket", 1, u2);
    r = WS("bind", 3, (u2 = WS("socket", 3, 2u, 2u, 0u)), sa(g_sa, 0, 9999), 16u);
    check("closesocket frees nothing of the first binder (still in use)", r == 0xFFFFFFFFu && err() == WSAEADDRINUSE, "err %u", err());
    WS("closesocket", 1, u2);

    printf("TCP in this browser\n");
    uint32_t L = WS("socket", 3, 2u, 1u, 0u); nb(L);
    r = WS("bind", 3, L, sa(g_sa, ipn(10, 77, 0, 2), 9900), 16u);
    r |= WS("listen", 2, L, 25u);
    check("listen on 10.77.0.2:9900", r == 0, "err %u", err());
    uint32_t C = WS("socket", 3, 2u, 1u, 0u); nb(C);
    r = WS("connect", 3, C, sa(g_sa, ipn(10, 77, 0, 2), 9900), 16u);
    check("non-blocking connect: WSAEWOULDBLOCK", r == 0xFFFFFFFFu && err() == WSAEWOULDBLOCK, "err %u", err());
    r = WS("select", 5, 0u, 0u, setof(1, 1, C), setof(2, 1, C), tv0());
    check("select: the connect completed (writable, not in exceptfds)", r == 1 && setn(1) == 1 && setn(2) == 0, "r=%d", (int)r);
    r = WS("select", 5, 0u, setof(0, 1, L), 0u, 0u, tv0());
    check("select: the listener is readable", r == 1 && setn(0) == 1, 0);
    uint32_t A = WS("accept", 3, L, GA(g_sa2), lenp(16));
    check("accept: the peer is 10.77.0.2:<C's port>", A != 0xFFFFFFFFu && sa_ip(g_sa2) == ipn(10, 77, 0, 2), "0x%x", A);
    WS("getsockname", 3, C, GA(g_sa), lenp(16));
    check("getsockname(C) = 10.77.0.2 and the port accept reported", sa_ip(g_sa) == ipn(10, 77, 0, 2) && sa_port(g_sa) == sa_port(g_sa2), 0);
    memcpy(g_buf, "@tic RC4+MD5-V2", 15);
    r = WS("send", 4, C, GA(g_buf), 15u, 0u);
    memset(g_buf2, 0, 64);
    uint32_t r2 = WS("recv", 4, A, GA(g_buf2), 100u, 0u);
    check("stream bytes C -> A", r == 15 && r2 == 15 && !memcmp(g_buf2, "@tic RC4+MD5-V2", 15), "r=%d r2=%d", (int)r, (int)r2);
    r = WS("recv", 4, A, GA(g_buf2), 100u, 0u);
    check("then WSAEWOULDBLOCK", r == 0xFFFFFFFFu && err() == WSAEWOULDBLOCK, 0);
    WS("send", 4, A, GA(g_buf), 4u, 0u);
    r = WS("recv", 4, C, GA(g_buf2), 2u, 2u);                        /* MSG_PEEK */
    r2 = WS("recv", 4, C, GA(g_buf2), 100u, 0u);
    check("A -> C, MSG_PEEK leaves the bytes", r == 2 && r2 == 4, "r=%d r2=%d", (int)r, (int)r2);
    WS("closesocket", 1, C);
    r = WS("recv", 4, A, GA(g_buf2), 100u, 0u);
    check("graceful close: the peer reads 0", r == 0, "r=%d err %u", (int)r, err());
    C = WS("socket", 3, 2u, 1u, 0u); nb(C);
    WS("connect", 3, C, sa(g_sa, ipn(127, 0, 0, 1), 9900), 16u);
    r = WS("listen", 2, L, 25u);
    uint32_t A2 = WS("accept", 3, L, GA(g_sa2), lenp(16));
    check("connect to 127.0.0.1 reaches a listener on our address? (no: bound to 10.77.0.2)", A2 == 0xFFFFFFFFu, "0x%x", A2);
    WS("getsockopt", 5, C, 0xFFFFu, 0x1007u, GA(g_opt), lenp(4));
    uint32_t soerr; memcpy(&soerr, g_opt, 4);
    check("... refused: SO_ERROR WSAECONNREFUSED", soerr == WSAECONNREFUSED, "%u", soerr);
    WS("getsockopt", 5, C, 0xFFFFu, 0x1007u, GA(g_opt), lenp(4)); memcpy(&soerr, g_opt, 4);
    check("SO_ERROR is cleared when read", soerr == 0, "%u", soerr);
    WS("closesocket", 1, C);
    C = WS("socket", 3, 2u, 1u, 0u); nb(C);
    WS("connect", 3, C, sa(g_sa, ipn(10, 77, 0, 2), 9900), 16u);
    WS("select", 5, 0u, 0u, setof(1, 1, C), 0u, tv0());
    A2 = WS("accept", 3, L, GA(g_sa2), lenp(16));
    g_opt[0] = 1; g_opt[1] = 0; g_opt[2] = 0; g_opt[3] = 0;           /* struct linger { 1, 0 }: abortive */
    WS("setsockopt", 5, A2, 0xFFFFu, 0x80u, GA(g_opt), 8u);
    WS("closesocket", 1, A2);
    r = WS("recv", 4, C, GA(g_buf2), 100u, 0u);
    check("linger {1,0} close: the peer's recv fails WSAECONNRESET", r == 0xFFFFFFFFu && err() == WSAECONNRESET, "r=%d err %u", (int)r, err());
    r = WS("select", 5, 0u, 0u, setof(1, 1, C), 0u, tv0());
    check("a reset socket is writable (Windows)", r == 1 && setn(1) == 1, 0);
    r = WS("getpeername", 3, C, GA(g_sa2), lenp(16));
    check("... and getpeername fails WSAENOTCONN (how DirtySock notices)", r == 0xFFFFFFFFu && err() == WSAENOTCONN, "err %u", err());
    WS("closesocket", 1, C); WS("closesocket", 1, A);

    printf("TCP to another browser\n");
    tx_drain();
    C = WS("socket", 3, 2u, 1u, 0u); nb(C);
    r = WS("connect", 3, C, sa(g_sa, ipn(10, 77, 0, 3), 9900), 16u);
    got = tx_take(&f);
    uint32_t cid = f.cid; uint16_t cport = f.sport;
    check("connect: a SYN to 10.77.0.3:9900", r == 0xFFFFFFFFu && err() == WSAEWOULDBLOCK && got && f.kind == NET_KIND_LAN_TCP &&
          f.op == NET_LAN_TCP_SYN && f.dst == PEER && f.dport == 9900 && cid != 0, "op %u cid %08x", f.op, cid);
    r = WS("select", 5, 0u, 0u, setof(1, 1, C), setof(2, 1, C), tv0());
    check("no answer yet: neither writable nor failed", r == 0, "r=%d", (int)r);
    rx_put(NET_KIND_LAN_TCP, PEER, ME, 9900, cport, NET_LAN_TCP_SYNACK, cid, 0, 0);
    r = WS("select", 5, 0u, 0u, setof(1, 1, C), setof(2, 1, C), tv0());
    check("SYNACK: connected (writable)", r == 1 && setn(1) == 1 && setn(2) == 0, "r=%d", (int)r);
    for (int i = 0; i < 3000; i++) g_buf[i] = (uint8_t)(i * 7);
    r = WS("send", 4, C, GA(g_buf), 3000u, 0u);
    int nd = 0, tot = 0, ok = 1;
    while (tx_take(&f)){ nd++; ok &= f.op == NET_LAN_TCP_DATA && f.cid == cid && !memcmp(f.d, g_buf + tot, (size_t)f.n); tot += f.n; }
    check("send 3000 bytes: DATA frames of at most 1464 bytes, in order", r == 3000 && nd == 3 && tot == 3000 && ok, "%d frames, %d bytes", nd, tot);
    rx_put(NET_KIND_LAN_TCP, PEER, ME, 9900, cport, NET_LAN_TCP_DATA, cid, "@dir REGN=", 10);
    rx_put(NET_KIND_LAN_TCP, PEER, ME, 9900, cport, NET_LAN_TCP_DATA, cid, "NA", 2);
    memset(g_buf2, 0, 64);
    r = WS("recv", 4, C, GA(g_buf2), 100u, 0u);
    check("two DATA frames read as one stream", r == 12 && !memcmp(g_buf2, "@dir REGN=NA", 12), "r=%d", (int)r);
    rx_put(NET_KIND_LAN_TCP, PEER, ME, 9900, cport, NET_LAN_TCP_DATA, 0x1234u, "zz", 2);
    got = tx_take(&f);
    check("DATA for an unknown connection id: RST back", got && f.op == NET_LAN_TCP_RST && f.cid == 0x1234u, 0);
    rx_put(NET_KIND_LAN_TCP, PEER, ME, 9900, cport, NET_LAN_TCP_FIN, cid, 0, 0);
    r = WS("recv", 4, C, GA(g_buf2), 100u, 0u);
    check("FIN: recv 0", r == 0, "r=%d", (int)r);
    WS("closesocket", 1, C);
    got = tx_take(&f);
    check("closesocket: our FIN", got && f.op == NET_LAN_TCP_FIN && f.cid == cid, "op %u", f.op);
    C = WS("socket", 3, 2u, 1u, 0u); nb(C);
    WS("connect", 3, C, sa(g_sa, ipn(10, 77, 0, 3), 9901), 16u);
    tx_take(&f); cid = f.cid; cport = f.sport;
    rx_put(NET_KIND_LAN_TCP, PEER, ME, 9901, cport, NET_LAN_TCP_RST, cid, 0, 0);
    r = WS("select", 5, 0u, 0u, setof(1, 1, C), setof(2, 1, C), tv0());
    WS("getsockopt", 5, C, 0xFFFFu, 0x1007u, GA(g_opt), lenp(4)); memcpy(&soerr, g_opt, 4);
    check("RST to a SYN: refused, in exceptfds only, SO_ERROR WSAECONNREFUSED", r == 1 && setn(2) == 1 && setn(1) == 0 && soerr == WSAECONNREFUSED,
          "r=%d soerr %u", (int)r, soerr);
    WS("closesocket", 1, C);
    tx_drain();
    C = WS("socket", 3, 2u, 1u, 0u); nb(C);
    WS("connect", 3, C, sa(g_sa, ipn(10, 77, 0, 7), 9900), 16u);
    check("connect to an address nobody holds: no SYN on the switch", tx_count() == 0, 0);
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    do { r = WS("select", 5, 0u, 0u, setof(1, 1, C), setof(2, 1, C), tv0()); clock_gettime(CLOCK_MONOTONIC, &t1); }
    while (!r && (t1.tv_sec - t0.tv_sec) < 5);
    double dt = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    WS("getsockopt", 5, C, 0xFFFFu, 0x1007u, GA(g_opt), lenp(4)); memcpy(&soerr, g_opt, 4);
    check("... fails after 3 s with WSAEHOSTUNREACH (vlanhub's rule)", r == 1 && setn(2) == 1 && soerr == WSAEHOSTUNREACH && dt > 2.8 && dt < 3.5,
          "%.2f s, %u", dt, soerr);
    WS("closesocket", 1, C);

    printf("TCP from another browser\n");
    tx_drain();
    rx_put(NET_KIND_LAN_TCP, PEER, ME, 50001, 9900, NET_LAN_TCP_SYN, 0xC0FFEEu, 0, 0);
    got = tx_take(&f);
    check("a SYN to our listener: SYNACK", got && f.op == NET_LAN_TCP_SYNACK && f.cid == 0xC0FFEEu && f.dst == PEER && f.dport == 50001, 0);
    rx_put(NET_KIND_LAN_TCP, PEER, ME, 50001, 9900, NET_LAN_TCP_DATA, 0xC0FFEEu, "@tic", 4);
    A = WS("accept", 3, L, GA(g_sa2), lenp(16));
    check("accept: 10.77.0.3:50001", A != 0xFFFFFFFFu && sa_ip(g_sa2) == ipn(10, 77, 0, 3) && sa_port(g_sa2) == 50001, 0);
    r = WS("recv", 4, A, GA(g_buf2), 100u, 0u);
    check("data that came before accept() is there", r == 4 && !memcmp(g_buf2, "@tic", 4), "r=%d", (int)r);
    WS("send", 4, A, GA(g_buf), 5u, 0u);
    got = tx_take(&f);
    check("send from the accepted socket: DATA to 10.77.0.3:50001", got && f.op == NET_LAN_TCP_DATA && f.dport == 50001 && f.sport == 9900 && f.n == 5, 0);
    rx_put(NET_KIND_LAN_TCP, PEER, ME, 50002, 9999, NET_LAN_TCP_SYN, 0xBEEFu, 0, 0);
    got = tx_take(&f);
    check("a SYN to a port nobody listens on: RST (refused)", got && f.op == NET_LAN_TCP_RST && f.cid == 0xBEEFu, 0);
    rx_put(NET_KIND_LAN_TCP, PEER, ME, 0, 0, NET_LAN_TCP_GONE, 0, 0, 0);
    r = WS("recv", 4, A, GA(g_buf2), 100u, 0u);
    check("the page says 10.77.0.3 left: its connection is reset", r == 0xFFFFFFFFu && err() == WSAECONNRESET, "r=%d err %u", (int)r, err());
    WS("closesocket", 1, A);
    rx_put(NET_KIND_LAN_TCP, PEER, ME, 50003, 9900, NET_LAN_TCP_SYN, 0xAAu, 0, 0);
    A = WS("accept", 3, L, GA(g_sa2), lenp(16));
    tx_drain();
    MB->page_state = NET_PAGE_OFFLINE; vlan_test_pump();
    r = WS("recv", 4, A, GA(g_buf2), 100u, 0u);
    check("the page lost the switch: remote connections are reset", r == 0xFFFFFFFFu && err() == WSAECONNRESET, "r=%d err %u", (int)r, err());
    r = WS("sendto", 6, u3, GA(g_buf), 9u, 0u, sa(g_sa, ipn(10, 77, 0, 3), 3658), 16u);
    check("UDP while offline: sent (dropped, as UDP may be), nothing queued", r == 9 && tx_count() == 0, 0);
    MB->generation = 2; MB->page_state = NET_PAGE_ONLINE; vlan_test_pump();
    WS("closesocket", 1, A);

    printf("ICMP (ProtoPing)\n");
    uint32_t P = WS("socket", 3, 2u, 3u, 1u); nb(P);
    check("socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)", P != 0xFFFFFFFFu, "err %u", err());
    uint8_t echo[16] = { 8, 0, 0, 0, 0x12, 0x34, 0, 1, 's', 'P', 'S', 'g', 1, 2, 3, 4 };
    memcpy(g_buf, echo, 16);
    r = WS("sendto", 6, P, GA(g_buf), 16u, 0u, sa(g_sa, ipn(10, 77, 0, 2), 0), 16u);
    memset(g_buf2, 0, 64);
    r2 = WS("recvfrom", 6, P, GA(g_buf2), 200u, 0u, GA(g_sa2), lenp(16));
    check("ping our own address: an echo reply with a 20-byte IPv4 header", r == 16 && r2 == 36 && g_buf2[0] == 0x45 && g_buf2[9] == 1 &&
          g_buf2[20] == 0 && !memcmp(g_buf2 + 28, "sPSg", 4) && sa_ip(g_sa2) == ipn(10, 77, 0, 2), "r=%d r2=%d", (int)r, (int)r2);
    tx_drain();
    WS("sendto", 6, P, GA(g_buf), 16u, 0u, sa(g_sa, ipn(10, 77, 0, 3), 0), 16u);
    got = tx_take(&f);
    check("ping 10.77.0.3: an ICMP frame", got && f.kind == NET_KIND_LAN_ICMP && f.dst == PEER && f.n == 16 && f.d[0] == 8, 0);
    rx_put(NET_KIND_LAN_ICMP, PEER, ME, 0, 0, 0, 0, echo, 16);
    got = tx_take(&f);
    check("an echo request from 10.77.0.3 is answered here", got && f.kind == NET_KIND_LAN_ICMP && f.d[0] == 0 && f.dst == PEER &&
          !memcmp(f.d + 4, echo + 4, 12), 0);
    r = WS("recvfrom", 6, P, GA(g_buf2), 200u, 0u, GA(g_sa2), lenp(16));
    check("... and the raw socket sees it too (Windows raw sockets see all ICMP)", r == 36 && g_buf2[20] == 8 && sa_ip(g_sa2) == ipn(10, 77, 0, 3), "r=%d", (int)r);

    printf("port unreachable goes to the socket that sent (two binders of a port)\n");
    {   /* A: b (second binder, SO_REUSEADDR, connected) sends; the notice is b's, not the first binder a's */
        uint32_t a = WS("socket", 3, 2u, 2u, 0u), b = WS("socket", 3, 2u, 2u, 0u); nb(a); nb(b);
        WS("bind", 3, a, sa(g_sa, 0, 7000), 16u);
        WS("setsockopt", 5, b, 0xFFFFu, 4u, one(1), 4u);
        r = WS("bind", 3, b, sa(g_sa, 0, 7000), 16u);
        WS("connect", 3, b, sa(g_sa, ipn(10, 77, 0, 3), 3658), 16u);
        tx_drain();
        r |= WS("send", 4, b, GA(g_buf), 4u, 0u) != 4u;
        got = tx_take(&f);
        check("b (second binder of 7000, connected) sends to 10.77.0.3:3658", r == 0 && got && f.sport == 7000 && f.dport == 3658, 0);
        rx_put(NET_KIND_LAN_UDP, PEER, ME, 3658, 7000, NET_LAN_UDP_UNREACH, 0, 0, 0);
        r = WS("recv", 4, b, GA(g_buf2), 100u, 0u);
        check("the notice reaches b: WSAECONNRESET", r == 0xFFFFFFFFu && err() == WSAECONNRESET, "r=%d err %u", (int)r, err());
        WS("connect", 3, a, sa(g_sa, ipn(10, 77, 0, 3), 3658), 16u);
        r = WS("recv", 4, a, GA(g_buf2), 100u, 0u);
        check("... and nothing reached the first binder a", r == 0xFFFFFFFFu && err() == WSAEWOULDBLOCK, "r=%d err %u", (int)r, err());
        WS("closesocket", 1, a); WS("closesocket", 1, b);
    }
    {   /* C: a2 is connected to :5000 and never sent; b2 probes :6000 from the same port */
        uint32_t a2 = WS("socket", 3, 2u, 2u, 0u), b2 = WS("socket", 3, 2u, 2u, 0u); nb(a2); nb(b2);
        WS("bind", 3, a2, sa(g_sa, 0, 7001), 16u);
        WS("setsockopt", 5, b2, 0xFFFFu, 4u, one(1), 4u);
        WS("bind", 3, b2, sa(g_sa, 0, 7001), 16u);
        WS("connect", 3, a2, sa(g_sa, ipn(10, 77, 0, 3), 5000), 16u);
        WS("sendto", 6, b2, GA(g_buf), 5u, 0u, sa(g_sa, ipn(10, 77, 0, 3), 6000), 16u);
        tx_drain();
        rx_put(NET_KIND_LAN_UDP, PEER, ME, 6000, 7001, NET_LAN_UDP_UNREACH, 0, 0, 0);
        r = WS("recv", 4, a2, GA(g_buf2), 100u, 0u);
        check("a notice for b2's probe does not reset a2's link to :5000", r == 0xFFFFFFFFu && err() == WSAEWOULDBLOCK, "r=%d err %u", (int)r, err());
        WS("closesocket", 1, a2); WS("closesocket", 1, b2);
    }
    {   /* D: two connected binders; and a send long forgotten: the connected socket of that peer */
        uint32_t d0 = WS("socket", 3, 2u, 2u, 0u), d1 = WS("socket", 3, 2u, 2u, 0u), d2 = WS("socket", 3, 2u, 2u, 0u);
        nb(d0); nb(d1); nb(d2);
        WS("bind", 3, d0, sa(g_sa, 0, 7002), 16u);
        WS("setsockopt", 5, d1, 0xFFFFu, 4u, one(1), 4u); WS("bind", 3, d1, sa(g_sa, 0, 7002), 16u);
        WS("setsockopt", 5, d2, 0xFFFFu, 4u, one(1), 4u); WS("bind", 3, d2, sa(g_sa, 0, 7002), 16u);
        WS("connect", 3, d1, sa(g_sa, ipn(10, 77, 0, 3), 5001), 16u);
        WS("connect", 3, d2, sa(g_sa, ipn(10, 77, 0, 3), 5002), 16u);
        WS("send", 4, d1, GA(g_buf), 3u, 0u); WS("send", 4, d2, GA(g_buf), 3u, 0u);
        rx_put(NET_KIND_LAN_UDP, PEER, ME, 5002, 7002, NET_LAN_UDP_UNREACH, 0, 0, 0);
        r = WS("recv", 4, d2, GA(g_buf2), 100u, 0u); uint32_t e2 = err();
        r2 = WS("recv", 4, d1, GA(g_buf2), 100u, 0u); uint32_t e1 = err();
        check("two connected binders: the one that sent to :5002 is reset, the other not", r == 0xFFFFFFFFu && e2 == WSAECONNRESET &&
              r2 == 0xFFFFFFFFu && e1 == WSAEWOULDBLOCK, "d2 err %u, d1 err %u", e2, e1);
        for (int i = 0; i < 40; i++) WS("sendto", 6, d1, GA(g_buf), 3u, 0u, sa(g_sa, ipn(10, 77, 0, 3), (uint16_t)(6100 + i)), 16u);
        tx_drain();
        rx_put(NET_KIND_LAN_UDP, PEER, ME, 5001, 7002, NET_LAN_UDP_UNREACH, 0, 0, 0);
        r = WS("recv", 4, d1, GA(g_buf2), 100u, 0u);
        check("its send to :5001 forgotten since: the socket connected to :5001 still gets it", r == 0xFFFFFFFFu && err() == WSAECONNRESET,
              "r=%d err %u", (int)r, err());
        WS("closesocket", 1, d0); WS("closesocket", 1, d1); WS("closesocket", 1, d2);
    }

    printf("control frames after a full TX ring\n");
    {   /* one large send to another browser: the FIN of closesocket still goes out */
        tx_drain();
        C = WS("socket", 3, 2u, 1u, 0u); nb(C);
        WS("connect", 3, C, sa(g_sa, ipn(10, 77, 0, 3), 9900), 16u);
        tx_take(&f); cid = f.cid; cport = f.sport;
        rx_put(NET_KIND_LAN_TCP, PEER, ME, 9900, cport, NET_LAN_TCP_SYNACK, cid, 0, 0);
        WS("select", 5, 0u, 0u, setof(1, 1, C), 0u, tv0());          /* connected */
        static uint8_t big[8192];
        int total = 0;
        for (int i = 0; i < 40; i++){ memset(big, i, sizeof big); r = WS("send", 4, C, GA(big), 8192u, 0u); if (r == 0xFFFFFFFFu) break; total += (int)r; }
        uint32_t eb = err(); int inring = tx_count();
        r = WS("select", 5, 0u, 0u, setof(1, 1, C), 0u, tv0());
        check("stream sends until WSAEWOULDBLOCK leave TX slots free; then not writable", eb == WSAEWOULDBLOCK && inring < (int)NET_TX_SLOTS &&
              total > 100000 && r == 0, "%d bytes, %d/%u slots, select %d", total, inring, NET_TX_SLOTS, (int)r);
        WS("closesocket", 1, C);
        int nfin = 0, ndata = 0, last = -1;
        while (tx_take(&f)){ if (f.op == NET_LAN_TCP_FIN && f.cid == cid) nfin++; else if (f.op == NET_LAN_TCP_DATA) ndata++; last = f.op; }
        check("closesocket: the FIN follows the data at once", nfin == 1 && ndata == inring && last == NET_LAN_TCP_FIN, "%d DATA, %d FIN", ndata, nfin);
    }
    {   /* datagrams fill the ring to the last slot: the FIN waits and goes out first once there is room */
        tx_drain();
        C = WS("socket", 3, 2u, 1u, 0u); nb(C);
        WS("connect", 3, C, sa(g_sa, ipn(10, 77, 0, 3), 9900), 16u);
        tx_take(&f); cid = f.cid; cport = f.sport;
        rx_put(NET_KIND_LAN_TCP, PEER, ME, 9900, cport, NET_LAN_TCP_SYNACK, cid, 0, 0);
        WS("select", 5, 0u, 0u, setof(1, 1, C), 0u, tv0());
        for (int i = 0; i < (int)NET_TX_SLOTS + 8; i++) WS("sendto", 6, u3, GA(g_buf), 9u, 0u, sa(g_sa, ipn(10, 77, 0, 3), 3658), 16u);
        int full = tx_count();
        WS("closesocket", 1, C);
        int nfin = 0;
        for (uint32_t i = MB->tx_tail; i != MB->tx_head; i++) if (slot(MB->tx_off, NET_TX_SLOTS, i)->data[0] == NET_LAN_TCP_FIN &&
                                                               slot(MB->tx_off, NET_TX_SLOTS, i)->kind == NET_KIND_LAN_TCP) nfin++;
        check("a full ring: closesocket's FIN waits (not lost, not sent)", full == (int)NET_TX_SLOTS && tx_count() == full && nfin == 0, "%d/%u, %d FIN", full, NET_TX_SLOTS, nfin);
        tx_take(&f);                                                  /* the page sent one: one slot free */
        WS("sendto", 6, u3, GA(g_buf), 9u, 0u, sa(g_sa, ipn(10, 77, 0, 3), 3658), 16u);   /* a later frame must not take it */
        vlan_test_pump();
        int n = 0, fin_at = -1;
        while (tx_take(&f)){ if (f.kind == NET_KIND_LAN_TCP && f.op == NET_LAN_TCP_FIN && f.cid == cid) fin_at = n; n++; }
        check("one slot free again: the FIN takes it, before any later frame", fin_at == (int)NET_TX_SLOTS - 1 && n == (int)NET_TX_SLOTS,
              "FIN at %d of %d", fin_at, n);
        WS("sendto", 6, u3, GA(g_buf), 9u, 0u, sa(g_sa, ipn(10, 77, 0, 3), 3658), 16u);
        vlan_test_pump();
        check("... and nothing is sent twice", tx_take(&f) && f.kind == NET_KIND_LAN_UDP && tx_count() == 0, 0);
    }

    printf("answers this host gives by itself: limited per sender and in all (the security review)\n");
    {   /* someone on the switch sends stray frames as fast as it can: echo requests of 1400 bytes, datagrams to
         * a port nobody holds, SYNs to a port nobody listens on, DATA of no connection.  Each was answered, and
         * the answers spent this browser's share of the switch, past which its own frames were dropped */
        tx_drain();
        static uint8_t ping[1400]; memset(ping, 0, sizeof ping); ping[0] = 8;
        const uint32_t X = 0x0A4D0010u, Y = 0x0A4D0011u;            /* 10.77.0.16, .17 */
        Frame g; int back = 0, bytes = 0;
        for (int i = 0; i < 100; i++){
            rx_put(NET_KIND_LAN_ICMP, X, ME, 0, 0, 0, 0, ping, sizeof ping);
            rx_put(NET_KIND_LAN_UDP, X, ME, 5000, 4444, 0, 0, "x", 1);
            rx_put(NET_KIND_LAN_TCP, X, ME, 50100, 9999, NET_LAN_TCP_SYN, 0x7000u + (uint32_t)i, 0, 0);
            rx_put(NET_KIND_LAN_TCP, X, ME, 50101, 9900, NET_LAN_TCP_DATA, 0x8000u + (uint32_t)i, "zz", 2);
            while (tx_take(&g)){ back++; bytes += (int)g.len + 64; }
        }
        check("400 stray frames from one sender: answers stop at its 8 KB", back > 0 && bytes <= 2 * 4096 + 200, "%d answers, %d bytes", back, bytes);
        rx_put(NET_KIND_LAN_ICMP, Y, ME, 0, 0, 0, 0, echo, 16);
        int ans = 0; while (tx_take(&g)) if (g.kind == NET_KIND_LAN_ICMP && g.dst == Y && g.d[0] == 0) ans++;
        check("... and another sender's ping is still answered", ans == 1, "%d", ans);
        back = 0; bytes = 0;
        for (uint32_t h = 0x20; h < 0x40; h++) for (int i = 0; i < 10; i++){
            rx_put(NET_KIND_LAN_ICMP, 0x0A4D0000u | h, ME, 0, 0, 0, 0, ping, sizeof ping);
            while (tx_take(&g)){ back++; bytes += (int)g.len + 64; }
        }
        check("32 senders together: answers stop at 64 KB for all", back > 0 && bytes <= 2 * 32768 + 500, "%d answers, %d bytes", back, bytes);
    }

    printf("cleanup\n");
    r = WS("WSACleanup", 0, 0u);
    check("WSACleanup closes every socket", r == 0, 0);
    r = WS("socket", 3, 2u, 2u, 0u);
    check("after the last WSACleanup: WSANOTINITIALISED", r == 0xFFFFFFFFu && err() == WSANOTINITIALISED, "err %u", err());
    check("... and no network session: the mailbox no longer wants the switch", MB->want == 0, "want %u", MB->want);

    printf("the page letting go of the switch, and a /21\n");
    fflush(stdout); fprintf(stderr, "lan_unit: the page lets go\n");
    MB->page_state = NET_PAGE_IDLE; vlan_test_pump();
    { uint32_t bell = MB->tx_bell;
      r = WS("WSAStartup", 2, 0x0202u, GA(g_wsa));
      check("WSAStartup while the page has let go and does not come back in 5 s: fails, no session",
            r != 0 && MB->want == 0 && MB->tx_bell == bell + 2, "r=%u want %u bells %u", r, MB->want, MB->tx_bell - bell); }
    /* still connecting again (after an idle the switch gives another address): no session on the old one */
    MB->page_state = NET_PAGE_CONNECTING; vlan_test_pump();
    r = WS("WSAStartup", 2, 0x0202u, GA(g_wsa));
    check("WSAStartup while the page is still connecting again, 5 s: fails, no session on the old address",
          r != 0 && MB->want == 0, "r=%u want %u", r, MB->want);
    /* the page is back with another address, on 10.77.0.0/21 */
    MB->vip = 0x0A4D0611u; MB->netmask = 0xFFFFF800u; MB->generation = 3;              /* 10.77.6.17 */
    memset(MB->peer_bits, 0, sizeof MB->peer_bits);
    MB->peer_bits[0x611 >> 5] |= 1u << (0x611 & 31); MB->peer_bits[0x709 >> 5] |= 1u << (0x709 & 31);   /* us, 10.77.7.9 */
    MB->page_state = NET_PAGE_ONLINE; vlan_test_pump();
    r = WS("WSAStartup", 2, 0x0202u, GA(g_wsa));
    check("WSAStartup with the page back: the new address taken, the switch wanted", r == 0 && MB->want == 1, "r=%u want %u", r, MB->want);
    r = WS("gethostname", 2, GA(g_buf), 64u);
    check("gethostname = VLAN-1553, the host number on the /21 (10.77.6.17)", r == 0 && !strcmp((char *)g_buf, "VLAN-1553"), "\"%s\"", g_buf);
    { uint32_t c = WS("socket", 3, 2u, 1u, 0u); nb(c); tx_drain();
      WS("connect", 3, c, sa(g_sa, ipn(10, 77, 7, 9), 5000), 16u);
      Frame g; int got2 = tx_take(&g);
      check("connect to 10.77.7.9 (its bit set, another /24, past a /22): a SYN goes out", got2 && g.op == NET_LAN_TCP_SYN && g.dst == 0x0A4D0709u, "dst %08x", g.dst);
      uint32_t c2 = WS("socket", 3, 2u, 1u, 0u); nb(c2); tx_drain();
      WS("connect", 3, c2, sa(g_sa, ipn(10, 77, 1, 9), 5000), 16u);
      check("connect to 10.77.1.9 (no bit): nobody there, nothing sent", tx_count() == 0, "%d frames", tx_count());
      uint32_t c3 = WS("socket", 3, 2u, 1u, 0u); nb(c3); tx_drain();
      WS("connect", 3, c3, sa(g_sa, ipn(10, 77, 5, 9), 5000), 16u);
      check("connect to 10.77.5.9 (no bit, past a /22): nobody there, nothing sent", tx_count() == 0, "%d frames", tx_count());
      WS("closesocket", 1, c); WS("closesocket", 1, c2); WS("closesocket", 1, c3); tx_drain(); }
    { uint32_t u = WS("socket", 3, 2u, 2u, 0u); nb(u);
      WS("setsockopt", 5, u, 0xFFFFu, 0x20u, one(1), 4u);
      WS("bind", 3, u, sa(g_sa, 0, 9999), 16u); tx_drain();
      memcpy(g_buf, "bcast", 5);
      WS("sendto", 6, u, GA(g_buf), 5u, 0u, sa(g_sa, ipn(10, 77, 7, 255), 9999), 16u);
      Frame g; int got2 = tx_take(&g);
      check("sendto 10.77.7.255, the /21's broadcast: a broadcast", got2 && g.dst == NET_BROADCAST, "dst %08x", g.dst);
      tx_drain();
      WS("sendto", 6, u, GA(g_buf), 5u, 0u, sa(g_sa, ipn(10, 77, 3, 255), 9999), 16u);
      got2 = tx_take(&g);
      check("sendto 10.77.3.255: a host of the /21 (the /22's broadcast), so unicast", got2 && g.dst == 0x0A4D03FFu, "dst %08x", g.dst);
      WS("closesocket", 1, u); tx_drain(); }
    r = WS("WSACleanup", 0, 0u);
    check("the last WSACleanup again: the switch no longer wanted", r == 0 && MB->want == 0, "want %u", MB->want);
    /* the page closes its link at exit (web/test/lan-wasm.test.mjs reads the log line after this marker) */
    fflush(stdout); fprintf(stderr, "lan_unit: the page closes the link\n");
    MB->page_state = NET_PAGE_NONE; vlan_test_pump();

    printf("lan_unit: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
