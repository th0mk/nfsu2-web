/* Most Wanted's overlapped Winsock (TRACKER M8-06, M8-07) as lifted code calls
 * it: arguments pushed right to left, a return address, then win32_call with
 * the import's index.  runtime/winsock.c over the browser's virtual LAN
 * (runtime/web/net_lan.c) and the Win32 layer's objects (runtime/win32.c,
 * win32_ext.c, win32_misc.c, keyboard.c) are the real ones; the rest of the
 * runtime is stubbed (rt_stubs.h).  The program plays the page: it writes
 * this browser's address into the LAN mailbox (no LAN thread; datagrams go
 * between its own sockets, which never leave the browser).
 *
 *   wsaov_unit.wasm hle      single-threaded (node's WASI, web/test/wsaov-wasm.test.mjs):
 *                            WSA events (create, set, reset, close; the objects WaitFor*
 *                            and CreateEventA share), WSAWaitForMultipleEvents' answers,
 *                            WSARecv / WSARecvFrom with an event OVERLAPPED (pending,
 *                            at once, truncated, scattered, in order, cancelled on close,
 *                            failed), WSAGetOverlappedResult, the LAN mailbox's want word
 *                            over a session with receives pending, and the call sequence of
 *                            DirtySock's socket thread (GAME.md 3.3); every call's stack
 *                            balanced (the table's stdcall counts: argcount's KNOWN)
 *   wsaov_unit.wasm stress   the waits with threads (the browser host, web/tools/wsastress.sh):
 *                            a WSASetEvent ping-pong, an event closed mid-wait, the guest
 *                            lock free during a wait, a datagram waking a waiter, the socket
 *                            thread's loop under a stream of datagrams, a socket closed under
 *                            a wait, WSAGetOverlappedResult's fWait; every wait finite, a lost
 *                            wake-up is a timeout and fails
 *
 * Prints a line per failed check and "wsaov_unit: <n> passed, <m> failed". */
#include "cpu.h"
#include "game.h"
#include "kbd.h"
#include "module.h"
#include "winsock.h"
#include "net_proto.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static _Atomic int pass, fail;
#define CHECK(cond, ...) do { if (cond) pass++; else { fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while (0)

/* ---- the import table: Windows' stdcall argument counts (SDK signatures, GAME.md 3.3) ---- */
const ImpEnt g_imp[] = {
#define IAT(i) (0x00500000u + 4u * (i))
    { IAT(0),  "WS2_32.dll",   "WSAStartup", 2 },
    { IAT(1),  "WS2_32.dll",   "WSACleanup", 0 },
    { IAT(2),  "WS2_32.dll",   "WSAGetLastError", 0 },
    { IAT(3),  "WS2_32.dll",   "socket", 3 },
    { IAT(4),  "WS2_32.dll",   "bind", 3 },
    { IAT(5),  "WS2_32.dll",   "closesocket", 1 },
    { IAT(6),  "WS2_32.dll",   "sendto", 6 },
    { IAT(7),  "WS2_32.dll",   "ioctlsocket", 3 },
    { IAT(8),  "WS2_32.dll",   "getsockname", 3 },
    { IAT(9),  "WS2_32.dll",   "connect", 3 },
    { IAT(10), "WS2_32.dll",   "listen", 2 },
    { IAT(11), "WS2_32.dll",   "accept", 3 },
    { IAT(12), "WS2_32.dll",   "send", 4 },
    { IAT(13), "WS2_32.dll",   "WSACreateEvent", 0 },
    { IAT(14), "WS2_32.dll",   "WSACloseEvent", 1 },
    { IAT(15), "WS2_32.dll",   "WSASetEvent", 1 },
    { IAT(16), "WS2_32.dll",   "WSAResetEvent", 1 },
    { IAT(17), "WS2_32.dll",   "WSAWaitForMultipleEvents", 5 },
    { IAT(18), "WS2_32.dll",   "WSARecv", 7 },
    { IAT(19), "WS2_32.dll",   "WSARecvFrom", 9 },
    { IAT(20), "WS2_32.dll",   "WSAGetOverlappedResult", 5 },
    { IAT(21), "KERNEL32.dll", "CreateEventA", 4 },
    { IAT(22), "KERNEL32.dll", "SetEvent", 1 },
    { IAT(23), "KERNEL32.dll", "CloseHandle", 1 },
    { IAT(24), "KERNEL32.dll", "WaitForSingleObject", 2 },
    { IAT(25), "KERNEL32.dll", "CreateMutexA", 3 },
};
const uint32_t g_imp_count = sizeof g_imp / sizeof *g_imp;

/* ---- guest memory: [0, the program's data) is the guest's (the link's global base) ---- */
uint8_t g_cbits[1u << 15];
#define STACK_TOP  0x00800000u           /* the main thread's guest stack; threads below it */
#define TEB_MAIN   0x00700000u
#define SCRATCH    0x00600000u
#define RET_ADDR   0x00401234u

/* ---- runtime stand-ins: what the linked files call outside them ---- */
const GameOps g_game = { .id = "test", .exe_name = "GAME.EXE", .vroot = "C:\\GAME", .module_path = "C:\\GAME\\GAME.EXE" };
#define RT_STUBS_WINSOCK_REAL            /* winsock.c is linked */
#include "rt_stubs.h"
void online_lobby_read(uint32_t link, const uint8_t *p, int n){ (void)link; (void)p; (void)n; }   /* online.c */
void olfilter_lobby_mark(uint32_t link, uint8_t *p, int n){ (void)link; (void)p; (void)n; }       /* olfilter.c */
void olfilter_lobby_sent(uint32_t link, uint8_t *p, int n){ (void)link; (void)p; (void)n; }

/* ---- the runtime's entry points the test drives ---- */
int win32_call(CPU *c, uint32_t idx);
void win32_init(void);
void guest_lock(CPU *c);
void guest_unlock(void);
net_mailbox *vlan_test_mailbox(void);

/* ---- calling an import as lifted code does ---- */
typedef struct { CPU cpu; uint32_t stack_top; } Thr;
static uint32_t idx_of(const char *name){
    for (uint32_t i = 0; i < g_imp_count; i++) if (!strcmp(g_imp[i].name, name)) return i;
    printf("FAIL: no import %s in the table\n", name); exit(2);
}
static uint32_t call_imp(Thr *t, const char *name, ...){
    uint32_t i = idx_of(name), n = (uint32_t)g_imp[i].nargs, a[16];
    va_list ap; va_start(ap, name);
    for (uint32_t k = 0; k < n; k++) a[k] = va_arg(ap, uint32_t);
    va_end(ap);
    CPU *c = &t->cpu;
    uint32_t base = t->stack_top - 0x100u;
    for (uint32_t k = 0; k < 8; k++) MR32(base + 4u * k) = 0xC0DE0000u + k;   /* the caller's frame, above the arguments */
    c->esp = base;
    for (uint32_t k = n; k-- > 0; ) PUSH32(a[k]);
    PUSH32(RET_ADDR);
    win32_call(c, i);
    int ok = 1;
    for (uint32_t k = 0; k < 8; k++) ok &= MR32(base + 4u * k) == 0xC0DE0000u + k;
    CHECK(c->esp == base, "%s: esp 0x%08x after the call, 0x%08x expected (%u arguments)", name, c->esp, base, n);
    CHECK(c->eip == RET_ADDR, "%s: return address 0x%08x", name, c->eip);
    CHECK(ok, "%s: wrote into the caller's frame", name);
    return c->eax;
}
static uint32_t last_error(Thr *t){ return MR32(t->cpu.fs_base + 0x34u); }
static void set_last_error(Thr *t, uint32_t e){ MR32(t->cpu.fs_base + 0x34u) = e; }
static void thr_init(Thr *t, uint32_t teb, uint32_t stack_top, uint32_t tid){
    memset(t, 0, sizeof *t);
    t->stack_top = stack_top;
    t->cpu.fs_base = teb; t->cpu.fcw = 0x027f;
    memset(g_mem + teb, 0, 0x1000);
    MR32(teb + 0x18u) = teb; MR32(teb + 0x24u) = tid;
}
static double now_ms(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }
#define SE 0xFFFFFFFFu                   /* SOCKET_ERROR / INVALID_SOCKET / WSA_WAIT_FAILED / WSA_INFINITE */
#define INF 0xFFFFFFFFu
#define WAIT_TIMEOUT 0x102u
#define WAIT_FAILED 0xFFFFFFFFu
#define ST_PENDING 0x103u                /* STATUS_PENDING */
#define ST_CANCELLED 0xC0000120u
#define ST_OVERFLOW 0x80000005u          /* STATUS_BUFFER_OVERFLOW */

/* ---- guest data ---- */
static uint32_t G(uint32_t off){ return SCRATCH + off; }
static uint32_t ipn(int a, int b, int c, int d){ return (uint32_t)a | (uint32_t)b << 8 | (uint32_t)c << 16 | (uint32_t)d << 24; }
#define ME_N ipn(10, 77, 0, 2)
static uint32_t sa(uint32_t p, uint32_t ip, uint16_t port){
    memset(g_mem + p, 0, 16); MR16(p) = 2; MR8(p + 2) = (uint8_t)(port >> 8); MR8(p + 3) = (uint8_t)port; MR32(p + 4) = ip; return p; }
static uint16_t sa_port(uint32_t p){ return (uint16_t)(MR8(p + 2) << 8 | MR8(p + 3)); }
static uint32_t harr(uint32_t p, int n, ...){
    va_list ap; va_start(ap, n);
    for (int i = 0; i < n; i++) MR32(p + 4u * (uint32_t)i) = va_arg(ap, uint32_t);
    va_end(ap); return p;
}
static uint32_t u32p(uint32_t p, uint32_t v){ MR32(p) = v; return p; }
/* an OVERLAPPED at p with hEvent ev (Internal, InternalHigh, Offset, OffsetHigh zero) */
static uint32_t ovl(uint32_t p, uint32_t ev){ memset(g_mem + p, 0, 20); MR32(p + 16u) = ev; return p; }
static uint32_t wsabuf(uint32_t p, uint32_t len, uint32_t buf){ MR32(p) = len; MR32(p + 4u) = buf; return p; }
static void fill(uint32_t p, int n, int seed){ for (int i = 0; i < n; i++) MR8(p + (uint32_t)i) = (uint8_t)(seed + 7 * i); }
static int same(uint32_t p, int n, int seed){ for (int i = 0; i < n; i++) if (MR8(p + (uint32_t)i) != (uint8_t)(seed + 7 * i)) return 0; return 1; }
static uint32_t wfme(Thr *t, uint32_t n, uint32_t arr, int all, uint32_t ms){
    return call_imp(t, "WSAWaitForMultipleEvents", n, arr, (uint32_t)all, ms, 0u);
}

/* ---- the page: this browser on the server's switch, without a LAN thread ---- */
static net_mailbox *MB;
static void page_online(void){
    MB = vlan_test_mailbox();
    MB->vip = 0x0A4D0002u; MB->netmask = 0xFFFFFF00u; MB->generation = 1;
    MB->peer_bits[0] = 1u << 2; MB->page_state = NET_PAGE_ONLINE;
}
/* a UDP socket bound to port (0: any), non-blocking, as DirtySock's SocketOpen leaves them */
static uint32_t udp(Thr *t, uint16_t port){
    uint32_t s = call_imp(t, "socket", 2u, 2u, 0u);
    call_imp(t, "ioctlsocket", s, 0x8004667Eu, u32p(G(0xF000), 1));
    if (call_imp(t, "bind", s, sa(G(0xF010), 0, port), 16u)) printf("FAIL: bind %u: %u\n", port, last_error(t));
    return s;
}
static uint16_t port_of(Thr *t, uint32_t s){
    u32p(G(0xF030), 16); call_imp(t, "getsockname", s, G(0xF040), G(0xF030)); return sa_port(G(0xF040));
}
static uint32_t send_to(Thr *t, uint32_t s, uint16_t port, int n, int seed){
    uint32_t b = G(0xE000); fill(b, n, seed);
    return call_imp(t, "sendto", s, b, (uint32_t)n, 0u, sa(G(0xF050), ME_N, port), 16u);
}

/* ======================= M8-06: WSA events ======================= */
static void test_events(Thr *t){
    /* ws2_32 makes and waits on events without a WSAStartup (it is CreateEvent / WaitForMultipleObjectsEx) */
    uint32_t e1 = call_imp(t, "WSACreateEvent"), e2 = call_imp(t, "WSACreateEvent"), e3 = call_imp(t, "WSACreateEvent");
    CHECK(e1 >= 0x00a00000u && e1 < 0x00b00000u && e2 != e1 && e3 != e2, "WSACreateEvent: handles of the object table (0x%08x 0x%08x 0x%08x)", e1, e2, e3);
    uint32_t a1 = harr(G(0x100), 1, e1);
    CHECK(wfme(t, 1, a1, 0, 0) == WAIT_TIMEOUT, "a new event is not signaled");
    CHECK(call_imp(t, "WaitForSingleObject", e1, 0u) == WAIT_TIMEOUT, "... to WaitForSingleObject either (one object table)");
    set_last_error(t, 0x55u);
    CHECK(call_imp(t, "WSASetEvent", e1) == 1, "WSASetEvent: TRUE");
    CHECK(last_error(t) == 0x55u, "... the last error untouched (%u)", last_error(t));
    CHECK(wfme(t, 1, a1, 0, 0) == 0 && wfme(t, 1, a1, 0, 0) == 0, "signaled, and still after a wait: manual reset");
    CHECK(call_imp(t, "WaitForSingleObject", e1, 0u) == 0, "WaitForSingleObject sees it set");
    CHECK(call_imp(t, "WSAResetEvent", e1) == 1 && wfme(t, 1, a1, 0, 0) == WAIT_TIMEOUT, "WSAResetEvent: TRUE, not signaled");
    call_imp(t, "SetEvent", e1);
    CHECK(wfme(t, 1, a1, 0, 0) == 0, "kernel32's SetEvent on a WSA event: the wait sees it");
    call_imp(t, "WSAResetEvent", e1);

    /* CreateEventA's auto-reset event with WSA events, as MW's socket thread waits: index 0 */
    uint32_t wake = call_imp(t, "CreateEventA", 0u, 0u, 0u, 0u);
    uint32_t a4 = harr(G(0x110), 4, wake, e1, e2, e3);
    CHECK(wfme(t, 4, a4, 0, 0) == WAIT_TIMEOUT, "four objects, none set: WSA_WAIT_TIMEOUT (0x102)");
    CHECK(call_imp(t, "WSASetEvent", wake) == 1 && wfme(t, 4, a4, 0, 0) == 0, "WSASetEvent on CreateEventA's event: index 0");
    CHECK(wfme(t, 4, a4, 0, 0) == WAIT_TIMEOUT, "... auto reset: the wait took it");
    call_imp(t, "WSASetEvent", e3); call_imp(t, "WSASetEvent", e2);
    CHECK(wfme(t, 4, a4, 0, 0) == 2, "two set: the lowest index (2, not 3)");
    CHECK(wfme(t, 4, a4, 1, 0) == WAIT_TIMEOUT, "wait for all, two of four set: WSA_WAIT_TIMEOUT");
    call_imp(t, "WSASetEvent", e1); call_imp(t, "WSASetEvent", wake);
    CHECK(wfme(t, 4, a4, 1, 0) == 0, "wait for all, all set: WSA_WAIT_EVENT_0");
    CHECK(wfme(t, 1, harr(G(0x130), 1, wake), 0, 0) == WAIT_TIMEOUT && wfme(t, 3, harr(G(0x140), 3, e1, e2, e3), 1, 0) == 0,
          "... it took the auto-reset one and left the manual ones set");
    call_imp(t, "WSAResetEvent", e1); call_imp(t, "WSAResetEvent", e2); call_imp(t, "WSAResetEvent", e3);
    call_imp(t, "WSASetEvent", e2);
    CHECK(wfme(t, 2, harr(G(0x150), 2, e2, e2), 0, 0) == 0, "the same event twice, wait for any: allowed");
    set_last_error(t, 0);
    CHECK(wfme(t, 2, harr(G(0x150), 2, e2, e2), 1, 0) == WAIT_FAILED && last_error(t) == WSA_INVALID_PARAMETER,
          "the same event twice, wait for all: WSA_WAIT_FAILED, WSA_INVALID_PARAMETER (%u)", last_error(t));

    /* the parameters */
    set_last_error(t, 0);
    CHECK(wfme(t, 0, a1, 0, 0) == WAIT_FAILED && last_error(t) == WSA_INVALID_PARAMETER, "cEvents 0: WSA_INVALID_PARAMETER (%u)", last_error(t));
    set_last_error(t, 0);
    CHECK(wfme(t, 65, G(0x200), 0, 0) == WAIT_FAILED && last_error(t) == WSA_INVALID_PARAMETER, "cEvents 65 (> WSA_MAXIMUM_WAIT_EVENTS): WSA_INVALID_PARAMETER");
    set_last_error(t, 0);
    CHECK(wfme(t, 1, 0u, 0, 0) == WAIT_FAILED && last_error(t) == WSA_INVALID_PARAMETER, "lphEvents NULL: WSA_INVALID_PARAMETER");
    set_last_error(t, 0);
    CHECK(wfme(t, 2, harr(G(0x160), 2, e1, 0x1234u), 0, 0) == WAIT_FAILED && last_error(t) == WSA_INVALID_HANDLE,
          "a value that is no object: WSA_WAIT_FAILED, WSA_INVALID_HANDLE (%u)", last_error(t));
    { uint32_t a64 = G(0x400); for (uint32_t i = 0; i < 64; i++) MR32(a64 + 4u * i) = i == 63 ? e2 : e1;
      CHECK(wfme(t, 64, a64, 0, 0) == 63, "64 objects (WSA_MAXIMUM_WAIT_EVENTS), the last set: 63"); }

    /* a timeout is real time, and the call comes back after it */
    double t0 = now_ms();
    uint32_t r = wfme(t, 1, harr(G(0x170), 1, e3), 0, 40);
    double el = now_ms() - t0;
    CHECK(r == WAIT_TIMEOUT && el >= 39.0 && el < 1000.0, "a 40 ms timeout: 0x%x after %.1f ms", r, el);

    /* closing */
    set_last_error(t, 0);
    CHECK(call_imp(t, "WSACloseEvent", e3) == 1, "WSACloseEvent: TRUE");
    CHECK(wfme(t, 1, harr(G(0x170), 1, e3), 0, 0) == WAIT_FAILED && last_error(t) == WSA_INVALID_HANDLE, "a closed event: WSA_INVALID_HANDLE");
    set_last_error(t, 0);
    CHECK(call_imp(t, "WSACloseEvent", e3) == 0 && last_error(t) == WSA_INVALID_HANDLE, "closed twice: FALSE, WSA_INVALID_HANDLE");
    set_last_error(t, 0);
    CHECK(call_imp(t, "WSASetEvent", e3) == 0 && last_error(t) == WSA_INVALID_HANDLE, "WSASetEvent on it: FALSE, WSA_INVALID_HANDLE");
    set_last_error(t, 0);
    CHECK(call_imp(t, "WSAResetEvent", e3) == 0 && last_error(t) == WSA_INVALID_HANDLE, "WSAResetEvent on it: FALSE, WSA_INVALID_HANDLE");
    uint32_t mx = call_imp(t, "CreateMutexA", 0u, 0u, 0u);
    set_last_error(t, 0);
    CHECK(call_imp(t, "WSASetEvent", mx) == 0 && last_error(t) == WSA_INVALID_HANDLE, "WSASetEvent on a mutex: not an event object");
    set_last_error(t, 0);
    CHECK(call_imp(t, "WSACloseEvent", mx) == 0 && last_error(t) == WSA_INVALID_HANDLE, "WSACloseEvent on a mutex: not an event object");
    CHECK(wfme(t, 1, harr(G(0x180), 1, mx), 0, 0) == 0, "the wait takes any object: a free mutex is ready");
    call_imp(t, "CloseHandle", mx);
    /* a handle closed and made again names the new event: a wait that began on the old one does not see it (the
     * stress half closes one under a wait); the new one starts not signaled */
    uint32_t e4 = call_imp(t, "WSACreateEvent");
    CHECK(wfme(t, 1, harr(G(0x190), 1, e4), 0, 0) == WAIT_TIMEOUT, "a new event in a freed slot (0x%08x, was 0x%08x): not signaled", e4, e3);
    call_imp(t, "WSACloseEvent", e1); call_imp(t, "WSACloseEvent", e2); call_imp(t, "WSACloseEvent", e4);
    call_imp(t, "CloseHandle", wake);
}

/* ======================= M8-07: overlapped receives ======================= */
/* DirtySock's socket record (GAME.md 3.3): the OVERLAPPED at +0x50 (hEvent +0x60), the from address +0x88, its
 * length +0xA0, the flags +0xA4, the byte count +0x98, the error +0x9C, "pending" +0xA8, a 576-byte buffer +0xA9 */
#define R_OV 0x50u
#define R_EV 0x60u
#define R_FROM 0x88u
#define R_RECVLEN 0x98u
#define R_ERR 0x9Cu
#define R_FROMLEN 0xA0u
#define R_FLAGS 0xA4u
#define R_PEND 0xA8u
#define R_BUF 0xA9u
static uint32_t recvfrom_ov(Thr *t, uint32_t s, uint32_t rec, uint32_t wb, uint32_t len){
    wsabuf(wb, len, rec + R_BUF);
    MR32(rec + R_FLAGS) = 0; MR32(rec + R_FROMLEN) = 16;
    return call_imp(t, "WSARecvFrom", s, wb, 1u, rec + R_RECVLEN, rec + R_FLAGS, rec + R_FROM, rec + R_FROMLEN, rec + R_OV, 0u);
}
static uint32_t ov_result(Thr *t, uint32_t s, uint32_t ov, uint32_t pcb, int wait, uint32_t pfl){
    return call_imp(t, "WSAGetOverlappedResult", s, ov, pcb, (uint32_t)wait, pfl);
}
static void test_overlapped(Thr *t){
    uint32_t rec = G(0x2000), wb = G(0x3000), cb = G(0x3100), fl = G(0x3110);
    memset(g_mem + rec, 0, 0x300);
    set_last_error(t, 0);
    CHECK(recvfrom_ov(t, 0x1000u, rec, wb, 576) == SE && last_error(t) == WSANOTINITIALISED, "WSARecvFrom before WSAStartup: WSANOTINITIALISED");
    page_online();
    CHECK(call_imp(t, "WSAStartup", 0x0202u, G(0x1000)) == 0, "WSAStartup(2.2) on the virtual LAN");
    CHECK(MB->want == 1, "a network session: the mailbox wants the switch (%u)", MB->want);
    uint32_t u = udp(t, 3658), s = udp(t, 0);
    uint16_t sport = port_of(t, s);
    uint32_t ev = call_imp(t, "WSACreateEvent"), wake = call_imp(t, "CreateEventA", 0u, 0u, 0u, 0u);
    MR32(rec + R_EV) = ev;
    uint32_t aw = harr(G(0x500), 2, wake, ev), a1 = harr(G(0x510), 1, ev);

    /* pending */
    call_imp(t, "WSASetEvent", ev);                                  /* the call clears it first */
    MR32(rec + R_RECVLEN) = 0x7777u;
    uint32_t r = recvfrom_ov(t, u, rec, wb, 576);
    CHECK(r == SE && last_error(t) == WSA_IO_PENDING, "WSARecvFrom, nothing there: SOCKET_ERROR, WSA_IO_PENDING (%u)", last_error(t));
    CHECK(MR32(rec + R_OV) == ST_PENDING && MR32(rec + R_OV + 4u) == 0, "Internal STATUS_PENDING, InternalHigh 0");
    CHECK(wfme(t, 1, a1, 0, 0) == WAIT_TIMEOUT, "hEvent cleared at the call");
    CHECK(MR32(rec + R_RECVLEN) == 0x7777u, "lpNumberOfBytesRecvd not written for a pending receive");
    set_last_error(t, 0);
    CHECK(ov_result(t, u, rec + R_OV, cb, 0, fl) == 0 && last_error(t) == WSA_IO_INCOMPLETE, "WSAGetOverlappedResult: FALSE, WSA_IO_INCOMPLETE (%u)", last_error(t));
    CHECK(wfme(t, 2, aw, 0, 10) == WAIT_TIMEOUT, "WSAWaitForMultipleEvents with nothing arriving: WSA_WAIT_TIMEOUT");
    wsabuf(wb, 0, 0xDEAD0000u);                                     /* the caller's stack, reused: the WSABUF was taken at the call */
    CHECK(send_to(t, s, 3658, 100, 1) == 100, "a datagram of 100 bytes to our own port 3658");
    CHECK(wfme(t, 2, aw, 0, 1000) == 1, "WSAWaitForMultipleEvents: the receive completes, its event (index 1)");
    CHECK(MR32(rec + R_OV) == 0 && MR32(rec + R_OV + 4u) == 100, "Internal 0, InternalHigh 100 (0x%x, %u)", MR32(rec + R_OV), MR32(rec + R_OV + 4u));
    CHECK(same(rec + R_BUF, 100, 1), "the data is in the buffer the WSABUF named at the call");
    CHECK(MR32(rec + R_FROM + 4u) == ME_N && sa_port(rec + R_FROM) == sport && MR16(rec + R_FROM) == 2 && MR32(rec + R_FROMLEN) == 16,
          "lpFrom 10.77.0.2:%u, lpFromlen 16 (port %u)", sport, sa_port(rec + R_FROM));
    MR32(fl) = 0x55u; MR32(cb) = 0;
    CHECK(ov_result(t, u, rec + R_OV, cb, 0, fl) == 1 && MR32(cb) == 100 && MR32(fl) == 0, "WSAGetOverlappedResult: TRUE, 100 bytes, flags 0");
    CHECK(MR32(rec + R_RECVLEN) == 0x7777u, "lpNumberOfBytesRecvd still not written (DirtySock reads the count from WSAGetOverlappedResult)");

    /* at once */
    send_to(t, s, 3658, 64, 2);
    r = recvfrom_ov(t, u, rec, wb, 576);
    CHECK(r == 0 && MR32(rec + R_RECVLEN) == 64 && MR32(rec + R_FLAGS) == 0, "data there: 0, 64 bytes, flags 0 (r=%x)", r);
    CHECK(MR32(rec + R_OV) == 0 && MR32(rec + R_OV + 4u) == 64 && same(rec + R_BUF, 64, 2) && sa_port(rec + R_FROM) == sport,
          "... Internal 0, InternalHigh 64, the data and lpFrom");
    CHECK(wfme(t, 1, a1, 0, 0) == 0, "... and hEvent set");

    /* truncated: at once, then pending */
    send_to(t, s, 3658, 100, 3); send_to(t, s, 3658, 8, 4);
    set_last_error(t, 0);
    r = recvfrom_ov(t, u, rec, wb, 10);
    CHECK(r == SE && last_error(t) == WSAEMSGSIZE && MR32(rec + R_RECVLEN) == 10, "a 10-byte buffer for 100: SOCKET_ERROR, WSAEMSGSIZE, 10 bytes");
    CHECK(MR32(rec + R_OV) == ST_OVERFLOW && MR32(rec + R_OV + 4u) == 10 && same(rec + R_BUF, 10, 3) && wfme(t, 1, a1, 0, 0) == 0,
          "... Internal STATUS_BUFFER_OVERFLOW, InternalHigh 10, the head, hEvent set");
    set_last_error(t, 0); MR32(cb) = 0;
    CHECK(ov_result(t, u, rec + R_OV, cb, 0, fl) == 0 && last_error(t) == WSAEMSGSIZE && MR32(cb) == 10, "WSAGetOverlappedResult: FALSE, WSAEMSGSIZE, 10 bytes");
    r = recvfrom_ov(t, u, rec, wb, 576);
    CHECK(r == 0 && MR32(rec + R_RECVLEN) == 8 && same(rec + R_BUF, 8, 4), "the rest was dropped: the next receive is the next datagram");
    CHECK(recvfrom_ov(t, u, rec, wb, 10) == SE && last_error(t) == WSA_IO_PENDING, "a 10-byte receive pending");
    send_to(t, s, 3658, 30, 5);
    set_last_error(t, 0); MR32(cb) = 0;
    CHECK(ov_result(t, u, rec + R_OV, cb, 0, fl) == 0 && last_error(t) == WSAEMSGSIZE && MR32(cb) == 10 && same(rec + R_BUF, 10, 5),
          "... it completes truncated: FALSE, WSAEMSGSIZE, 10 bytes (%u, %u)", last_error(t), MR32(cb));

    /* scattered over three WSABUFs; WSARecv (no lpFrom) */
    { uint32_t w3 = G(0x3200), b1 = G(0x3300), b2 = G(0x3340), b3 = G(0x3380), ov = ovl(G(0x3400), ev), nr = G(0x3420), f3 = G(0x3424);
      wsabuf(w3, 4, b1); wsabuf(w3 + 8, 6, b2); wsabuf(w3 + 16, 100, b3); MR32(f3) = 0;
      memset(g_mem + b1, 0, 0x200);
      r = call_imp(t, "WSARecv", u, w3, 3u, nr, f3, ov, 0u);
      CHECK(r == SE && last_error(t) == WSA_IO_PENDING, "WSARecv with three WSABUFs: pending");
      fill(G(0xE000), 20, 6); call_imp(t, "sendto", s, G(0xE000), 20u, 0u, sa(G(0xF050), ME_N, 3658), 16u);
      MR32(cb) = 0;
      CHECK(ov_result(t, u, ov, cb, 0, fl) == 1 && MR32(cb) == 20, "... completes with 20 bytes");
      int ok = 1; for (int i = 0; i < 20; i++){ uint32_t p = i < 4 ? b1 + (uint32_t)i : i < 10 ? b2 + (uint32_t)(i - 4) : b3 + (uint32_t)(i - 10);
                                                ok &= MR8(p) == (uint8_t)(6 + 7 * i); }
      CHECK(ok && MR8(b3 + 10) == 0, "... spread over the buffers in order: 4, 6, then 10 bytes"); }

    /* two receives pending on one socket complete in the order they were made */
    { uint32_t ov1 = ovl(G(0x3500), ev), ov2 = ovl(G(0x3520), 0), w1 = wsabuf(G(0x3540), 64, G(0x3600)), w2 = wsabuf(G(0x3550), 64, G(0x3700));
      uint32_t f1 = u32p(G(0x3560), 0), f2 = u32p(G(0x3564), 0);
      CHECK(call_imp(t, "WSARecv", u, w1, 1u, 0u, f1, ov1, 0u) == SE && last_error(t) == WSA_IO_PENDING &&
            call_imp(t, "WSARecv", u, w2, 1u, 0u, f2, ov2, 0u) == SE && last_error(t) == WSA_IO_PENDING, "two receives pending (one without hEvent)");
      send_to(t, s, 3658, 11, 7); send_to(t, s, 3658, 22, 8);
      CHECK(ov_result(t, u, ov2, cb, 0, fl) == 1 && MR32(cb) == 22 && same(G(0x3700), 22, 8), "the second gets the second datagram");
      CHECK(ov_result(t, u, ov1, cb, 0, fl) == 1 && MR32(cb) == 11 && same(G(0x3600), 11, 7), "the first gets the first"); }

    /* a non-blocking socket changes nothing; no OVERLAPPED is a plain receive */
    { uint32_t w1 = wsabuf(G(0x3800), 64, G(0x3900)), nr = G(0x3810), f1 = u32p(G(0x3814), 0);
      set_last_error(t, 0);
      CHECK(call_imp(t, "WSARecv", u, w1, 1u, nr, f1, 0u, 0u) == SE && last_error(t) == WSAEWOULDBLOCK, "no OVERLAPPED, non-blocking, nothing there: WSAEWOULDBLOCK");
      send_to(t, s, 3658, 33, 9);
      CHECK(call_imp(t, "WSARecv", u, w1, 1u, nr, f1, 0u, 0u) == 0 && MR32(nr) == 33 && same(G(0x3900), 33, 9), "... with data: 0, 33 bytes"); }

    /* MSG_PEEK: the datagram stays */
    { uint32_t ov = ovl(G(0x3A00), ev), w1 = wsabuf(G(0x3A20), 64, G(0x3B00)), f1 = u32p(G(0x3A30), 2);
      send_to(t, s, 3658, 12, 10);
      CHECK(call_imp(t, "WSARecv", u, w1, 1u, G(0x3A34), f1, ov, 0u) == 0 && MR32(G(0x3A34)) == 12 && MR32(f1) == 0, "MSG_PEEK: 12 bytes, flags 0 out");
      MR32(f1) = 0;
      CHECK(call_imp(t, "WSARecv", u, w1, 1u, G(0x3A34), f1, ov, 0u) == 0 && MR32(G(0x3A34)) == 12 && same(G(0x3B00), 12, 10), "... and the datagram is still there"); }

    /* refused at once: the OVERLAPPED and hEvent untouched */
    { uint32_t ov = ovl(G(0x3C00), ev), w1 = wsabuf(G(0x3C20), 64, G(0x3D00)), f1 = u32p(G(0x3C30), 0);
      uint32_t ub = call_imp(t, "socket", 2u, 2u, 0u);
      call_imp(t, "WSAResetEvent", ev); MR32(ov) = 0x1234u;
      set_last_error(t, 0);
      CHECK(call_imp(t, "WSARecv", ub, w1, 1u, 0u, f1, ov, 0u) == SE && last_error(t) == WSAEINVAL, "an unbound UDP socket: WSAEINVAL at once (%u)", last_error(t));
      CHECK(MR32(ov) == 0x1234u && wfme(t, 1, a1, 0, 0) == WAIT_TIMEOUT, "... Internal and hEvent untouched");
      set_last_error(t, 0);
      CHECK(call_imp(t, "WSARecv", 0x1234u, w1, 1u, 0u, f1, ov, 0u) == SE && last_error(t) == WSAENOTSOCK, "not a socket: WSAENOTSOCK");
      set_last_error(t, 0);
      CHECK(call_imp(t, "WSARecv", u, w1, 1u, 0u, 0u, ov, 0u) == SE && last_error(t) == WSAEFAULT, "lpFlags NULL: WSAEFAULT");
      set_last_error(t, 0); MR32(f1) = 1;
      CHECK(call_imp(t, "WSARecv", u, w1, 1u, 0u, f1, ov, 0u) == SE && last_error(t) == WSAEOPNOTSUPP, "MSG_OOB on a datagram socket: WSAEOPNOTSUPP");
      set_last_error(t, 0); MR32(f1) = 0;
      CHECK(call_imp(t, "WSARecv", u, w1, 1u, 0u, f1, ov, 0x00401000u) == SE && last_error(t) == WSAEOPNOTSUPP, "a completion routine (no APCs): WSAEOPNOTSUPP");
      set_last_error(t, 0); MR32(G(0x3C40)) = 8;
      CHECK(call_imp(t, "WSARecvFrom", u, w1, 1u, 0u, f1, G(0x3C50), G(0x3C40), ov, 0u) == SE && last_error(t) == WSAEFAULT, "lpFromlen 8: WSAEFAULT");
      uint32_t mx = call_imp(t, "CreateMutexA", 0u, 0u, 0u);
      set_last_error(t, 0); ovl(ov, mx);
      CHECK(call_imp(t, "WSARecv", u, w1, 1u, 0u, f1, ov, 0u) == SE && last_error(t) == WSA_INVALID_HANDLE, "hEvent a mutex: WSA_INVALID_HANDLE");
      call_imp(t, "CloseHandle", mx);
      set_last_error(t, 0);
      CHECK(call_imp(t, "WSARecv", u, w1, 0u, 0u, f1, ovl(ov, ev), 0u) == SE && last_error(t) == WSAEINVAL, "no WSABUF: WSAEINVAL");
      call_imp(t, "closesocket", ub); }

    /* a connected socket: the port-unreachable notice ends a pending receive with WSAECONNRESET */
    { uint32_t cs = udp(t, 0), ov = ovl(G(0x3E00), ev), w1 = wsabuf(G(0x3E20), 64, G(0x3F00)), f1 = u32p(G(0x3E30), 0);
      call_imp(t, "connect", cs, sa(G(0x3E40), ME_N, 4999), 16u);
      CHECK(call_imp(t, "WSARecv", cs, w1, 1u, 0u, f1, ov, 0u) == SE && last_error(t) == WSA_IO_PENDING, "WSARecv on a connected UDP socket: pending");
      fill(G(0xE000), 4, 0); call_imp(t, "send", cs, G(0xE000), 4u, 0u);         /* nobody on 4999 */
      set_last_error(t, 0);
      CHECK(ov_result(t, cs, ov, cb, 0, fl) == 0 && last_error(t) == WSAECONNRESET && MR32(ov) == 0xC000020Du,
            "port unreachable: FALSE, WSAECONNRESET, Internal STATUS_CONNECTION_RESET (%u, 0x%x)", last_error(t), MR32(ov));
      call_imp(t, "closesocket", cs); }

    /* a stream: WSARecv pending until the other end sends, a zero-byte receive, FIN */
    { uint32_t l = call_imp(t, "socket", 2u, 1u, 0u), c = call_imp(t, "socket", 2u, 1u, 0u);
      call_imp(t, "bind", l, sa(G(0xF010), 0, 9900), 16u); call_imp(t, "listen", l, 5u);
      call_imp(t, "connect", c, sa(G(0xF020), ME_N, 9900), 16u);
      uint32_t a = call_imp(t, "accept", l, 0u, 0u);
      CHECK(a != SE, "a local TCP connection (accept 0x%x, err %u)", a, last_error(t));
      uint32_t ov = ovl(G(0x4000), ev), w1 = wsabuf(G(0x4020), 64, G(0x4100)), f1 = u32p(G(0x4030), 0), w0 = wsabuf(G(0x4040), 0, 0);
      uint32_t ov0 = ovl(G(0x4050), 0);
      CHECK(call_imp(t, "WSARecv", a, w0, 1u, 0u, f1, ov0, 0u) == SE && last_error(t) == WSA_IO_PENDING, "a zero-byte WSARecv on a stream: pending");
      CHECK(call_imp(t, "WSARecv", a, w1, 1u, 0u, f1, ov, 0u) == SE && last_error(t) == WSA_IO_PENDING, "a 64-byte one behind it: pending");
      fill(G(0xE000), 40, 11); call_imp(t, "send", c, G(0xE000), 40u, 0u);
      CHECK(ov_result(t, a, ov0, cb, 0, fl) == 1 && MR32(cb) == 0, "the zero-byte receive completes when data is there (0 bytes)");
      CHECK(ov_result(t, a, ov, cb, 0, fl) == 1 && MR32(cb) == 40 && same(G(0x4100), 40, 11), "the next gets the 40 bytes");
      CHECK(call_imp(t, "WSARecv", a, w1, 1u, 0u, f1, ov, 0u) == SE && last_error(t) == WSA_IO_PENDING, "pending again");
      call_imp(t, "closesocket", c);
      CHECK(ov_result(t, a, ov, cb, 0, fl) == 1 && MR32(cb) == 0, "the other end closes: it completes with 0 bytes (FIN)");
      call_imp(t, "closesocket", a); call_imp(t, "closesocket", l); }

    /* fWait: data already there is a completion, not a wait */
    CHECK(recvfrom_ov(t, u, rec, wb, 576) == SE && last_error(t) == WSA_IO_PENDING, "pending again");
    send_to(t, s, 3658, 50, 12);
    MR32(cb) = 0;
    CHECK(ov_result(t, u, rec + R_OV, cb, 1, fl) == 1 && MR32(cb) == 50, "WSAGetOverlappedResult(fWait TRUE): TRUE, 50 bytes");

    /* closing the socket ends its pending receive: WSA_OPERATION_ABORTED, hEvent set */
    call_imp(t, "WSAResetEvent", ev);
    CHECK(recvfrom_ov(t, u, rec, wb, 576) == SE && last_error(t) == WSA_IO_PENDING, "pending");
    CHECK(call_imp(t, "closesocket", u) == 0, "closesocket");
    CHECK(MR32(rec + R_OV) == ST_CANCELLED && MR32(rec + R_OV + 4u) == 0, "Internal STATUS_CANCELLED, InternalHigh 0 (0x%x)", MR32(rec + R_OV));
    CHECK(wfme(t, 1, a1, 0, 0) == 0, "hEvent set");
    set_last_error(t, 0);
    CHECK(ov_result(t, u, rec + R_OV, cb, 0, fl) == 0 && last_error(t) == WSAENOTSOCK, "WSAGetOverlappedResult on the closed socket: WSAENOTSOCK");

    /* DirtySock's SocketClose: WSACloseEvent first, then closesocket.  The event made next takes the freed handle;
     * the cancelled receive does not set it (Windows sets the object it began with) */
    u = udp(t, 3658);
    call_imp(t, "WSAResetEvent", ev);
    CHECK(recvfrom_ov(t, u, rec, wb, 576) == SE && last_error(t) == WSA_IO_PENDING, "pending (event 0x%08x)", ev);
    call_imp(t, "WSACloseEvent", ev);
    uint32_t ev2 = call_imp(t, "WSACreateEvent");
    call_imp(t, "closesocket", u);
    CHECK(MR32(rec + R_OV) == ST_CANCELLED, "cancelled");
    CHECK(wfme(t, 1, harr(G(0x520), 1, ev2), 0, 0) == WAIT_TIMEOUT, "the new event in its place (0x%08x) is not set", ev2);
    ev = ev2; MR32(rec + R_EV) = ev; a1 = harr(G(0x510), 1, ev); aw = harr(G(0x500), 2, wake, ev);

    /* the last WSACleanup ends what is pending, and the mailbox no longer wants the switch; a new session does */
    u = udp(t, 3658);
    CHECK(recvfrom_ov(t, u, rec, wb, 576) == SE && last_error(t) == WSA_IO_PENDING, "pending");
    CHECK(call_imp(t, "WSACleanup") == 0 && MB->want == 0, "the last WSACleanup: want 0 (%u)", MB->want);
    CHECK(MR32(rec + R_OV) == ST_CANCELLED && wfme(t, 1, a1, 0, 0) == 0, "... the receive cancelled, hEvent set");
    CHECK(call_imp(t, "WSAStartup", 0x0202u, G(0x1000)) == 0 && MB->want == 1, "a new WSAStartup: want 1");
    call_imp(t, "closesocket", s);
    call_imp(t, "CloseHandle", wake);
    call_imp(t, "WSACloseEvent", ev);
}

/* ---- DirtySock's socket thread (speed.exe 0x84AC10 with 0x84AB40 and 0x84AAE0; server.dll 0x100043C0) ----
 * One round of its loop, with its records; got() is its callback, which reads the datagram the way
 * SocketRecvfrom (server.dll 0x10004840) does: the bytes out, recvlen 0, and a WSASetEvent on the wake event
 * unless the record's error is WSA_IO_PENDING. */
typedef struct { uint32_t rec, sock, ev; int got, bad; uint8_t last; } DsSock;
static void ds_issue(Thr *t, DsSock *d){                          /* 0x84AB40 */
    uint32_t rec = d->rec, wb = rec + 0x300u;                     /* its WSABUF lives on its stack: here, past the record */
    if (MR32(rec + R_RECVLEN) || MR8(rec + R_PEND) == 1) return;
    uint32_t r = recvfrom_ov(t, d->sock, rec, wb, 0x240);
    if (r){
        MR32(rec + R_RECVLEN) = r;
        uint32_t e = call_imp(t, "WSAGetLastError");
        MR32(rec + R_ERR) = e;
        if (e == WSA_IO_PENDING) MR8(rec + R_PEND) = 1;
    } else if ((int32_t)MR32(rec + R_RECVLEN) > 0) MR32(rec + R_ERR) = 0;   /* 0x84AAE0's callback comes next */
}
static void ds_consume(Thr *t, DsSock *d, uint32_t wake){           /* the callback: SocketRecvfrom */
    uint32_t rec = d->rec;
    int32_t n = (int32_t)MR32(rec + R_RECVLEN);
    if (n > 0){
        uint8_t seq = MR8(rec + R_BUF);
        if (d->got && seq != (uint8_t)(d->last + 1)) d->bad++;
        d->last = seq; d->got++;
        MR32(rec + R_ERR) = 0;
    }
    MR32(rec + R_RECVLEN) = 0;
    if (MR32(rec + R_ERR) != WSA_IO_PENDING){ MR32(rec + R_ERR) = 0; call_imp(t, "WSASetEvent", wake); }
}
/* one pass of the loop; returns WSAWaitForMultipleEvents' answer */
static uint32_t ds_round(Thr *t, DsSock *ds, int n, uint32_t wake, uint32_t arr, uint32_t ms){
    uint32_t cnt = 1;
    MR32(arr) = wake;
    for (int i = 0; i < n; i++){
        DsSock *d = &ds[i]; uint32_t rec = d->rec;
        if ((int32_t)MR32(rec + R_RECVLEN) > 0) continue;
        MR32(arr + 4u * cnt++) = d->ev;
        if (MR8(rec + R_PEND) == 1){
            uint32_t r = call_imp(t, "WSAGetOverlappedResult", d->sock, rec + R_OV, rec + R_RECVLEN, 0u, rec + R_FLAGS);
            if (r == 1){ MR8(rec + R_PEND) = 0; ds_consume(t, d, wake); }
            else if (call_imp(t, "WSAGetLastError") != WSA_IO_INCOMPLETE) MR8(rec + R_PEND) = 0;
        }
        ds_issue(t, d);
        if ((int32_t)MR32(rec + R_RECVLEN) > 0) ds_consume(t, d, wake);
    }
    uint32_t r = call_imp(t, "WSAWaitForMultipleEvents", cnt, arr, 0u, ms, 0u);
    if (r < cnt) call_imp(t, "WSAResetEvent", MR32(arr + 4u * r));
    return r;
}
static void ds_open(Thr *t, DsSock *d, uint32_t rec, uint16_t port){
    memset(d, 0, sizeof *d); d->rec = rec;
    memset(g_mem + rec, 0, 0x300);
    d->sock = udp(t, port);
    d->ev = call_imp(t, "WSACreateEvent");
    MR32(rec + R_EV) = d->ev;                                        /* the OVERLAPPED's hEvent */
}
static void test_dirtysock(Thr *t){
    DsSock ds[2];
    uint32_t wake = call_imp(t, "CreateEventA", 0u, 0u, 0u, 0u), arr = G(0x5000);
    ds_open(t, &ds[0], G(0x6000), 3658); ds_open(t, &ds[1], G(0x6400), 9999);
    uint32_t s = udp(t, 0);
    int bad = 0;
    /* each round: the receives issued, then 0 to 3 datagrams to each port; nothing is lost, in order */
    int seq[2] = { 0, 0 };
    for (int k = 0; k < 200; k++){
        uint32_t r = ds_round(t, ds, 2, wake, arr, 0);
        if (r == WAIT_FAILED) bad++;
        for (int i = 0; i < 2; i++) for (int m = 0; m < (k + i) % 4; m++){
            uint32_t b = G(0xE000); MR8(b) = (uint8_t)seq[i]++; memset(g_mem + b + 1, i, 15);
            call_imp(t, "sendto", s, b, 16u, 0u, sa(G(0xF050), ME_N, i ? 9999 : 3658), 16u);
        }
    }
    for (int k = 0; k < 400 && (ds[0].got < seq[0] || ds[1].got < seq[1]); k++) ds_round(t, ds, 2, wake, arr, 0);
    CHECK(!bad && ds[0].got == seq[0] && ds[1].got == seq[1] && !ds[0].bad && !ds[1].bad,
          "DirtySock's loop: %d and %d of %d and %d datagrams, in order (%d and %d out of order, %d failed waits)",
          ds[0].got, ds[1].got, seq[0], seq[1], ds[0].bad, ds[1].bad, bad);
    /* SocketClose: unlinked, WSACloseEvent, then shutdown and closesocket; the next round waits without it */
    call_imp(t, "WSACloseEvent", ds[1].ev);
    call_imp(t, "closesocket", ds[1].sock);
    CHECK(MR32(ds[1].rec + R_OV) == ST_CANCELLED, "SocketClose: its pending receive cancelled");
    CHECK(ds_round(t, ds, 1, wake, arr, 0) != WAIT_FAILED, "the loop goes on with the other socket");
    call_imp(t, "WSACloseEvent", ds[0].ev); call_imp(t, "closesocket", ds[0].sock); call_imp(t, "closesocket", s);
    call_imp(t, "CloseHandle", wake);
}

/* ======================= stress: the waits with threads ======================= */
typedef struct { Thr t; int id; uint32_t a, b, c, d; _Atomic int done; int n, bad; double t_ms, worst_ms; uint32_t r, e; } Job;
static Job g_job[8];
static uint32_t job_stack(int i){ return STACK_TOP - 0x20000u * (uint32_t)(i + 1); }
static uint32_t job_teb(int i){ return TEB_MAIN + 0x1000u * (uint32_t)(i + 1); }
static int wait_jobs(int from, int to, double ms){
    double t0 = now_ms();
    for (;;){
        int all = 1;
        for (int i = from; i < to; i++) all &= atomic_load(&g_job[i].done);
        if (all) return 1;
        if (now_ms() - t0 > ms) return 0;
        usleep(2000);
    }
}
static int start_job(int i, void *(*fn)(void *)){
    Job *j = &g_job[i];
    thr_init(&j->t, job_teb(i), job_stack(i), 200u + (uint32_t)i);
    j->id = i; atomic_store(&j->done, 0); j->n = j->bad = 0; j->t_ms = j->worst_ms = 0; j->r = j->e = 0;
    pthread_t th;
    if (pthread_create(&th, 0, fn, j)){ printf("FAIL: pthread_create\n"); fail++; return 0; }
    pthread_detach(th);
    return 1;
}
/* 1. ping-pong: each side waits on its CreateEventA event among WSA events and wakes the other with WSASetEvent */
#define PINGS 500
static void *ping_thread(void *u){
    Job *j = u; Thr *t = &j->t;
    const uint32_t arr = G(0x7000) + 0x20u * (uint32_t)j->id;
    guest_lock(&t->cpu);
    harr(arr, 3, j->b, j->a, j->c);                                   /* a WSA event, its own, another WSA event */
    for (int k = 0; k < PINGS; k++){
        if (j->id == 0) call_imp(t, "WSASetEvent", j->d);
        double t0 = now_ms();
        uint32_t r = call_imp(t, "WSAWaitForMultipleEvents", 3u, arr, 0u, 2000u, 0u);
        double el = now_ms() - t0;
        if (el > j->worst_ms) j->worst_ms = el;
        if (r != 1) j->bad++; else j->n++;
        if (j->id != 0) call_imp(t, "WSASetEvent", j->d);
    }
    guest_unlock();
    atomic_store(&j->done, 1);
    return 0;
}
/* 2. a waiter whose event is closed under it; 3. a waiter while another thread takes the guest lock */
static void *one_waiter(void *u){
    Job *j = u; Thr *t = &j->t;
    guest_lock(&t->cpu);
    double t0 = now_ms();
    j->r = call_imp(t, "WSAWaitForMultipleEvents", 1u, harr(G(0x7200) + 0x10u * (uint32_t)j->id, 1, j->a), 0u, INF, 0u);
    j->t_ms = now_ms();  (void)t0;
    j->e = last_error(t);
    guest_unlock();
    atomic_store(&j->done, 1);
    return 0;
}
/* 4. a pending receive and an INFINITE wait on its event, a datagram from another thread */
static void *recv_waiter(void *u){
    Job *j = u; Thr *t = &j->t;
    guest_lock(&t->cpu);
    uint32_t rec = G(0x8000), wb = G(0x8400);
    memset(g_mem + rec, 0, 0x300); MR32(rec + R_EV) = j->a;
    uint32_t r = recvfrom_ov(t, j->b, rec, wb, 576);
    if (r != SE || last_error(t) != WSA_IO_PENDING) j->bad++;
    atomic_store(&j->done, 2);                                       /* pending: the main thread may send */
    j->r = call_imp(t, "WSAWaitForMultipleEvents", 1u, harr(G(0x7300), 1, j->a), 0u, INF, 0u);
    j->t_ms = now_ms();
    j->n = (int)MR32(rec + R_OV + 4u); j->e = MR32(rec + R_OV);
    guest_unlock();
    atomic_store(&j->done, 1);
    return 0;
}
/* 5. DirtySock's socket thread under a stream of datagrams */
#define STREAM 3000
static _Atomic int g_ds_stop, g_ds_got;
static void *ds_thread(void *u){
    Job *j = u; Thr *t = &j->t;
    guest_lock(&t->cpu);
    DsSock d; memset(&d, 0, sizeof d);
    d.rec = G(0x9000); d.sock = j->b; d.ev = j->a;
    memset(g_mem + d.rec, 0, 0x300); MR32(d.rec + R_EV) = d.ev;
    while (!atomic_load(&g_ds_stop)){
        uint32_t r = ds_round(t, &d, 1, j->c, G(0x9400), INF);
        if (r == WAIT_FAILED) j->bad++;
        atomic_store(&g_ds_got, d.got);
    }
    j->n = d.got; j->e = (uint32_t)d.bad;
    guest_unlock();
    atomic_store(&j->done, 1);
    return 0;
}
/* 7. WSAGetOverlappedResult(fWait TRUE) in a thread */
static void *fwait_thread(void *u){
    Job *j = u; Thr *t = &j->t;
    guest_lock(&t->cpu);
    uint32_t rec = G(0xA000), wb = G(0xA400), cb = G(0xA500), fl = G(0xA504);
    memset(g_mem + rec, 0, 0x300); MR32(rec + R_EV) = j->a;
    if (recvfrom_ov(t, j->b, rec, wb, 576) != SE || last_error(t) != WSA_IO_PENDING) j->bad++;
    atomic_store(&j->done, 2);
    j->r = ov_result(t, j->b, rec + R_OV, cb, 1, fl);
    j->t_ms = now_ms(); j->n = (int)MR32(cb);
    guest_unlock();
    atomic_store(&j->done, 1);
    return 0;
}
static int wait_state(Job *j, int v, double ms){
    double t0 = now_ms();
    while (atomic_load(&j->done) != v){ if (now_ms() - t0 > ms) return 0; usleep(1000); }
    return 1;
}
static void test_stress(Thr *t){
    page_online();
    CHECK(call_imp(t, "WSAStartup", 0x0202u, G(0x1000)) == 0, "WSAStartup");
    guest_unlock();

    /* 1. ping-pong */
    guest_lock(&t->cpu);
    uint32_t w0 = call_imp(t, "CreateEventA", 0u, 0u, 0u, 0u), w1 = call_imp(t, "CreateEventA", 0u, 0u, 0u, 0u);
    uint32_t x0 = call_imp(t, "WSACreateEvent"), x1 = call_imp(t, "WSACreateEvent");
    guest_unlock();
    g_job[0].a = w0; g_job[0].d = w1; g_job[0].b = x0; g_job[0].c = x1;
    g_job[1].a = w1; g_job[1].d = w0; g_job[1].b = x1; g_job[1].c = x0;
    double t0 = now_ms();
    start_job(1, ping_thread); start_job(0, ping_thread);
    int ok = wait_jobs(0, 2, 60000.0);
    double el = now_ms() - t0;
    CHECK(ok && !g_job[0].bad && !g_job[1].bad && g_job[0].n == PINGS && g_job[1].n == PINGS,
          "WSASetEvent ping-pong: %d and %d of %d, %d and %d lost or wrong, %.0f ms", g_job[0].n, g_job[1].n, PINGS, g_job[0].bad, g_job[1].bad, el);
    /* a set wakes the waiter at once (winsock_wake): without that each wait would last its 50 ms slice */
    CHECK(el / PINGS < 5.0, "ping-pong: %.3f ms a round trip (a wait that only ends with its slice takes 50 ms)", el / PINGS);
    printf("  ping-pong: %d round trips in %.0f ms (%.3f ms each), worst wait %.1f ms\n", PINGS, el, el / PINGS,
           g_job[0].worst_ms > g_job[1].worst_ms ? g_job[0].worst_ms : g_job[1].worst_ms);

    /* 2. an event closed during an INFINITE wait: WSA_WAIT_FAILED, WSA_INVALID_HANDLE */
    g_job[2].a = x0;
    start_job(2, one_waiter);
    usleep(50000);
    guest_lock(&t->cpu); double tc = now_ms(); call_imp(t, "WSACloseEvent", x0); guest_unlock();
    ok = wait_jobs(2, 3, 5000.0);
    CHECK(ok && g_job[2].r == WAIT_FAILED && g_job[2].e == WSA_INVALID_HANDLE, "closed mid-wait: 0x%x, error %u (%s, %.1f ms after the close)",
          g_job[2].r, g_job[2].e, ok ? "returned" : "STUCK", g_job[2].t_ms - tc);
    printf("  closed mid-wait: WSA_WAIT_FAILED, error %u, %.2f ms after the WSACloseEvent\n", g_job[2].e, g_job[2].t_ms - tc);

    /* 3. the guest lock is free while a thread waits: this thread takes it at once, then wakes the waiter */
    g_job[3].a = x1;
    start_job(3, one_waiter);
    usleep(50000);
    double tl = now_ms();
    guest_lock(&t->cpu);
    double got_lock = now_ms() - tl;
    call_imp(t, "WSASetEvent", x1);
    double ts = now_ms();
    guest_unlock();
    ok = wait_jobs(3, 4, 5000.0);
    CHECK(got_lock < 100.0, "the guest lock while another thread waits: taken in %.2f ms", got_lock);
    CHECK(ok && g_job[3].r == 0, "WSASetEvent wakes the INFINITE wait: %u, %.2f ms after the set (%s)", g_job[3].r, g_job[3].t_ms - ts, ok ? "returned" : "STUCK");
    printf("  guest lock taken in %.2f ms while a thread waits; the set woke it %.2f ms later\n", got_lock, g_job[3].t_ms - ts);

    /* 4. a datagram wakes a thread whose receive is pending */
    guest_lock(&t->cpu);
    uint32_t ev = call_imp(t, "WSACreateEvent"), u = udp(t, 3658), s = udp(t, 0);
    guest_unlock();
    g_job[4].a = ev; g_job[4].b = u;
    start_job(4, recv_waiter);
    ok = wait_state(&g_job[4], 2, 5000.0);
    usleep(30000);
    guest_lock(&t->cpu); send_to(t, s, 3658, 77, 3); double tsend = now_ms(); guest_unlock();
    ok = ok && wait_jobs(4, 5, 5000.0);
    CHECK(ok && !g_job[4].bad && g_job[4].r == 0 && g_job[4].n == 77 && g_job[4].e == 0,
          "a datagram ends the wait: %u, %d bytes, Internal 0x%x, %.2f ms after the send (%s)", g_job[4].r, g_job[4].n, g_job[4].e,
          g_job[4].t_ms - tsend, ok ? "returned" : "STUCK");
    printf("  a datagram ended the wait %.2f ms after the send\n", g_job[4].t_ms - tsend);

    /* 5. DirtySock's socket thread: STREAM datagrams, sent as fast as it takes them (at most 200 ahead) */
    guest_lock(&t->cpu); uint32_t wake = call_imp(t, "CreateEventA", 0u, 0u, 0u, 0u); guest_unlock();
    g_job[5].a = ev; g_job[5].b = u; g_job[5].c = wake;
    atomic_store(&g_ds_stop, 0); atomic_store(&g_ds_got, 0);
    start_job(5, ds_thread);
    t0 = now_ms();
    int sent = 0, stuck = 0;
    while (sent < STREAM){
        if (sent - atomic_load(&g_ds_got) >= 200){
            double tw = now_ms();
            while (sent - atomic_load(&g_ds_got) >= 200 && now_ms() - tw < 3000.0) usleep(200);
            if (sent - atomic_load(&g_ds_got) >= 200){ stuck = 1; break; }
        }
        guest_lock(&t->cpu);
        uint32_t b = G(0xE000); MR8(b) = (uint8_t)sent; memset(g_mem + b + 1, 0x5A, 31);
        call_imp(t, "sendto", s, b, 32u, 0u, sa(G(0xF050), ME_N, 3658), 16u);
        guest_unlock();
        sent++;
    }
    double tw = now_ms();
    while (atomic_load(&g_ds_got) < sent && now_ms() - tw < 5000.0) usleep(500);
    el = now_ms() - t0;
    atomic_store(&g_ds_stop, 1);
    guest_lock(&t->cpu); call_imp(t, "WSASetEvent", wake); guest_unlock();
    ok = wait_jobs(5, 6, 5000.0);
    CHECK(ok && !stuck && g_job[5].n == STREAM && !g_job[5].e && !g_job[5].bad,
          "DirtySock's socket thread: %d of %d datagrams in order (%u out of order, %d failed waits, %s) in %.0f ms",
          g_job[5].n, STREAM, g_job[5].e, g_job[5].bad, stuck ? "STUCK" : ok ? "ended" : "did not end", el);
    printf("  socket thread: %d datagrams in %.0f ms (%.0f per second)\n", g_job[5].n, el, el > 0 ? g_job[5].n * 1000.0 / el : 0);

    /* 6. a socket closed under a wait on its pending receive's event: the receive cancelled, the event set */
    g_job[6].a = ev; g_job[6].b = u;
    start_job(6, recv_waiter);
    ok = wait_state(&g_job[6], 2, 5000.0);
    usleep(30000);
    guest_lock(&t->cpu); call_imp(t, "closesocket", u); double tcl = now_ms(); guest_unlock();
    ok = ok && wait_jobs(6, 7, 5000.0);
    CHECK(ok && g_job[6].r == 0 && g_job[6].e == ST_CANCELLED, "closesocket ends the wait: %u, Internal 0x%x, %.2f ms after (%s)",
          g_job[6].r, g_job[6].e, g_job[6].t_ms - tcl, ok ? "returned" : "STUCK");
    printf("  closesocket ended the wait %.2f ms later, the receive cancelled\n", g_job[6].t_ms - tcl);

    /* 7. WSAGetOverlappedResult(fWait TRUE) until another thread's datagram */
    guest_lock(&t->cpu); u = udp(t, 3658); guest_unlock();
    g_job[7].a = ev; g_job[7].b = u;
    start_job(7, fwait_thread);
    ok = wait_state(&g_job[7], 2, 5000.0);
    usleep(30000);
    guest_lock(&t->cpu); send_to(t, s, 3658, 91, 4); tsend = now_ms(); guest_unlock();
    ok = ok && wait_jobs(7, 8, 5000.0);
    CHECK(ok && !g_job[7].bad && g_job[7].r == 1 && g_job[7].n == 91, "fWait: TRUE, %d bytes, %.2f ms after the send (%s)",
          g_job[7].n, g_job[7].t_ms - tsend, ok ? "returned" : "STUCK");
    printf("  WSAGetOverlappedResult(fWait) came back %.2f ms after the send\n", g_job[7].t_ms - tsend);

    guest_lock(&t->cpu);
    call_imp(t, "closesocket", u); call_imp(t, "closesocket", s);
    call_imp(t, "WSACloseEvent", x1); call_imp(t, "WSACloseEvent", ev);
    call_imp(t, "CloseHandle", w0); call_imp(t, "CloseHandle", w1); call_imp(t, "CloseHandle", wake);
    call_imp(t, "WSACleanup");
}

int main(int argc, char **argv){
    const char *mode = argc > 1 ? argv[1] : "hle";
    setenv("NFSU2_LAN_NOTHREAD", "1", 1);                         /* the program is the page: no LAN thread */
    memset(g_cbits, 0xFF, 64u << 20 >> 14 >> 3);                    /* the first 64 MB are committed */
    win32_init();
    Thr t; thr_init(&t, TEB_MAIN, STACK_TOP, 100u);
    guest_lock(&t.cpu);
    if (!strcmp(mode, "hle")){
        test_events(&t);
        test_overlapped(&t);
        test_dirtysock(&t);
    } else if (!strcmp(mode, "stress")) test_stress(&t);
    else { printf("usage: wsaov_unit.wasm hle|stress\n"); return 2; }
    guest_unlock();
    printf("wsaov_unit: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
