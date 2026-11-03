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
#include <math.h>

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
void sub_005891b0_orig(CPU *c){ (void)c; }
void sub_005f8af0_orig(CPU *c){ (void)c; }
/* the game's handler of the creator's messages, as dispatch_indirect calls it */
static int g_calls; static uint32_t g_fn, g_len; static uint8_t g_bytes[64];
void dispatch_indirect(CPU *c, uint32_t addr);
#include "../runtime/racenet.c"
void dispatch_indirect(CPU *c, uint32_t addr){
    if (addr != FN_JOIN_MSG){ c->esp += 4u; return; }    /* the game's apply and the like: nothing here */
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

/* ---- the rates, the race clock, the cars where their owners have them ---- */
static uint32_t g_lcg = 12345u;
static float rnd01(void){ g_lcg = g_lcg * 1664525u + 1013904223u; return (float)(g_lcg >> 8) / 16777216.f; }
/* a creator's answer as the joiner's handler gets it: a bit stream, read
 * position at bit 8, our real-time clock echoed, its race clock */
#define BSTREAM 0x10050000u
static void answer(float f1, float f2){
    union { float f; uint32_t u; } a = { f1 }, b = { f2 };
    memset(g_mem + BSTREAM, 0, 0x410);
    for (uint32_t i = 0; i < 32; i++){
        uint32_t pa = 8 + i, pb = 40 + i;
        if (a.u >> (31 - i) & 1u) g_mem[BSTREAM + (pa >> 3)] |= (uint8_t)(0x80u >> (pa & 7));
        if (b.u >> (31 - i) & 1u) g_mem[BSTREAM + (pb >> 3)] |= (uint8_t)(0x80u >> (pb & 7));
    }
    MR32(BSTREAM + BS_READ) = 8;
    g_c.esp = STACK - 0x100u; MR32(g_c.esp + 4u) = BSTREAM;
    sub_005891b0(&g_c);
}
/* the wall clock (racenet.c's now_us: the fake clock) */
static void setwall(double r){ g_fake_ns = (uint64_t)(r * 1e9 + 0.5); }
/* a car state of ours to the creator (0x585D50's: 8 bits the message, 32 the
 * race clock, the slot's bits, 32 our real-time stamp, 8 the flags) */
#define BSOUT 0x10090000u
static void sent_state(float stamp){
    union { float f; uint32_t u; } a = { stamp };
    uint32_t nb = MR32(G_SLOTBITS), pos = 40u + nb;
    memset(g_mem + BSOUT, 0, 0x410);
    g_mem[BSOUT] = MSG_CARSPAM;
    for (uint32_t i = 0; i < 32; i++, pos++) if (a.u >> (31 - i) & 1u) g_mem[BSOUT + (pos >> 3)] |= (uint8_t)(0x80u >> (pos & 7));
    MR32(BSOUT + BS_BYTES) = 80u + nb;
    g_c.esp = STACK - 0x100u; MR32(g_c.esp + 4u) = MSG_CARSPAM; MR32(g_c.esp + 8u) = BSOUT; MR32(g_c.esp + 12u) = 0;
    sub_00585d50(&g_c);
}
#define CTL    0x10060000u
#define INPUT  0x10070000u
#define STATE  0x10080000u
static void put_state(float t, const float *p, const float *v, const float *w, const float *q, float brake){
    memset(g_mem + STATE, 0, 0x100);
    for (uint32_t k = 0; k < 3; k++){ MRF32(STATE + ST_POS + 4 * k) = p[k]; MRF32(STATE + ST_VEL + 4 * k) = v[k]; MRF32(STATE + ST_SPIN + 4 * k) = w[k]; }
    for (uint32_t k = 0; k < 4; k++) MRF32(STATE + ST_QUAT + 4 * k) = q[k];
    MRF32(STATE + ST_TIME) = t; MRF32(STATE + ST_BRAKE) = brake; MRF32(STATE + ST_GAS) = 0.9f;
    g_c.esp = STACK - 0x100u; MR32(g_c.esp + 4u) = STATE; g_c.ecx = CTL;
    sub_005f8900(&g_c);
}
static void blend(void){ g_c.esp = STACK - 0x100u; g_c.ecx = CTL; MRF32(INPUT + 0x214u) = 0.7f; sub_005f8af0(&g_c); }
static float dist3(const float *a, const float *b){ float d = 0; for (int i = 0; i < 3; i++) d += (a[i] - b[i]) * (a[i] - b[i]); return sqrtf(d); }
static void fidelity(void){
    uint32_t g[] = { G_RATES, G_CLK_TARGET, G_CLK_FLAG, G_CLK_ECHO, G_REALTIME, G_WORLDTIME_S };
    for (unsigned i = 0; i < sizeof g / sizeof g[0]; i++) commit(g[i], 32);
    commit(BSTREAM, 0x410); commit(BSOUT, 0x410); commit(CTL, 0x200); commit(INPUT, 0x220); commit(STATE, 0x100);
    MR32(G_NET_HOST) = 0; MR32(G_NETMODE) = 8;

    /* rates: every field group at RATE_HZ in a network race, the game's back after */
    MR32(G_NETMODE) = 1; frame(); MR32(G_NETMODE) = 8;   /* (the earlier cases raced with this page: its values back) */
    MR32(G_RATES) = 20; MR32(G_RATES + 4) = 10; MR32(G_RATES + 8) = 5; MR32(G_RATES + 12) = 5;
    frame();
    int on = MR32(G_RATES) == RATE_HZ && MR32(G_RATES + 4) == RATE_HZ && MR32(G_RATES + 12) == RATE_HZ;
    MR32(G_NETMODE) = 1; frame();
    int back = MR32(G_RATES) == 20 && MR32(G_RATES + 4) == 10 && MR32(G_RATES + 8) == 5 && MR32(G_RATES + 12) == 5;
    CHECK(on && back, "rates: %u a second for every field in a race (%d), the game's 20/10/5/5 after it (%d)", RATE_HZ, on, back);
    MR32(G_NETMODE) = 8;

    /* the race clock: answers whose round trips wait 0-80 ms on the way back
     * (a queue, the creator's frame) say the clock is up to 40 ms earlier;
     * the estimate takes the least waited ones.  Times on the wall clock;
     * our states' real-time stamps anything (the game's clock stands through
     * long frames): the round trip is from when the state went */
    const double theta = 7.25;                           /* the creator's clock minus our wall clock */
    double r = 100.0, worst_game = 0;
    float stamp = 1000.f;
    for (int i = 0; i < 90; i++){                        /* 3 s at 30 answers a second */
        double extra = rnd01() < 0.2 ? 0.0 : 0.08 * rnd01();
        double send = r - 0.1 - extra;                   /* our state left then: 50 ms up, 50 + extra down */
        stamp += 0.5f; MRF32(G_CLK_ECHO) = stamp - 0.25f;
        setwall(send); sent_state(stamp);
        setwall(r); answer(stamp, (float)(r - 0.05 - extra + theta));
        double one = (r - 0.05 - extra + theta) + 0.5 * (r - send) - r - theta;
        if (fabs(one) > worst_game) worst_game = fabs(one);
        r += 1.0 / 30.0;
    }
    CHECK(K.have && fabs(K.th - theta) < 0.004 && K.samples == 90,
          "clock: the estimate %.1f ms off (one sample, as the game took it: up to %.1f ms off)", (K.th - theta) * 1e3, worst_game * 1e3);
    /* an answer to a state we never sent (an older game's echo): taken by nobody */
    uint32_t ns = K.samples; MRF32(G_CLK_ECHO) = 1.f; setwall(r); answer(3.5f, (float)(r + theta));
    CHECK(K.samples == ns, "clock: an answer echoing no state of ours left out");
    /* restored: the game's own target and flag stay as they were */
    MR32(G_CLK_TARGET) = 1234u; MR8(G_CLK_FLAG) = 0;
    stamp += 0.5f; MRF32(G_CLK_ECHO) = stamp - 0.25f; setwall(r - 0.1); sent_state(stamp); setwall(r); answer(stamp, (float)(r - 0.05 + theta));
    CHECK(MR32(G_CLK_TARGET) == 1234u && MR8(G_CLK_FLAG) == 0, "clock: the game's target and flag untouched (%u %u)", MR32(G_CLK_TARGET), MR8(G_CLK_FLAG));
    /* the creator's clock loses 150 ms (its stall): followed within 10 answers */
    for (int i = 0; i < 10; i++){
        r += 1.0 / 30.0;
        stamp += 0.5f; MRF32(G_CLK_ECHO) = stamp - 0.25f; setwall(r - 0.1); sent_state(stamp);
        setwall(r); answer(stamp, (float)(r - 0.05 + theta - 0.15));
    }
    CHECK(fabs(K.th - (theta - 0.15)) < 0.004, "clock: a 150 ms step of the creator's clock followed (%.1f ms off)", (K.th - theta + 0.15) * 1e3);
    /* a creator (an older game) whose clock runs 7 % slow: it drifts */
    double th2 = theta - 0.15, worst = 0;
    for (int i = 0; i < 90; i++){
        r += 1.0 / 30.0; th2 -= 0.07 / 30.0;
        double extra = rnd01() < 0.2 ? 0.0 : 0.08 * rnd01();
        stamp += 0.5f; MRF32(G_CLK_ECHO) = stamp - 0.25f; setwall(r - 0.1 - extra); sent_state(stamp);
        setwall(r); answer(stamp, (float)(r - 0.05 - extra + th2));
        if (i >= 45 && fabs(K.th - th2) > worst) worst = fabs(K.th - th2);
    }
    CHECK(worst < 0.005 && fabs(K.drift + 0.07) < 0.02, "clock: a creator 7 %% slow followed (at most %.1f ms off, drift %.3f)", worst * 1e3, K.drift);
    K.drift = 0;
    /* WorldTimer to it: 50 ms off, frames of 8.3 ms: never over a fifth of a frame a frame, the world's step untouched */
    K.th = theta; K.at = r; K.frac = 0; K.irate = 0; K.wok = 0;
    double rr = r;
    setwall(rr);
    MR32(G_WORLDTIME) = (uint32_t)((rr + theta - 0.05) * 4000.0 + 0.5);
    MRF32(G_WORLDDT) = 0.0083f;
    clock_frame();
    int steady = 1; double left = 0; uint32_t prev = MR32(G_WORLDTIME);
    for (int f = 0; f < 240; f++){                       /* 2 s */
        rr += 0.0083; setwall(rr);
        uint32_t adv = 33u;                              /* the world's step: the frame's time */
        MR32(G_WORLDTIME) += adv; prev += adv;
        K.at = rr;
        clock_frame();
        int32_t d = (int32_t)(MR32(G_WORLDTIME) - prev);
        if (d > 7 || d < -7) steady = 0;
        prev = MR32(G_WORLDTIME);
        left = rr + theta - (double)MR32(G_WORLDTIME) * 0.00025;
    }
    CHECK(steady && fabs(left) < 0.001 && fabsf(MRF32(G_WORLDDT) - 0.0083f) < 1e-6f,
          "clock: WorldTimer to the estimate (%.2f ms left after 2 s, at most a fifth of a frame a frame %d), the step untouched", left * 1e3, steady);
    /* the game's catch-up: frames of 22 ms, each 0, 1 or 2 world steps of 16.7 ms
     * one right after the other (main-loop passes), the world 1 % slower than the wall
     * clock: WorldTimer around the wall clock, not run off (the per-pass top-up of
     * the time missing ran it 6 % fast) */
    {
        double acc = 0, h1 = 0, h2 = 0, mx = 0;
        for (int f = 0; f < 900; f++){                   /* 20 s */
            rr += 0.022; acc += 0.022 * 0.99;
            int steps = 0;
            while (acc >= 1.0 / 60 && steps < 3){ acc -= 1.0 / 60; steps++; }
            for (int k = 0; k < (steps ? steps : 1); k++){
                setwall(rr + 0.0001 * k);
                if (steps) MR32(G_WORLDTIME) += 67u;      /* 16.75 ms */
                K.at = rr; clock_frame();
            }
            double l = rr + theta - (double)MR32(G_WORLDTIME) * 0.00025;
            if (f >= 300 && f < 600) h1 += l / 300; else if (f >= 600) h2 += l / 300;
            if (f >= 300 && fabs(l) > mx) mx = fabs(l);
        }
        CHECK(fabs(h2 - h1) < 0.002 && fabs(h2) < 0.012 && mx < 0.03,
              "clock: through the game's catch-up steps, 1 %% slow: %.2f then %.2f ms off on average (a step's sawtooth), at most %.1f ms",
              h1 * 1e3, h2 * 1e3, mx * 1e3);
    }
    double lag = 0;
    for (int f = 0; f < 480; f++){
        rr += 0.0083; setwall(rr);
        MR32(G_WORLDTIME) += 31u;                        /* 7.75 ms of the frame's 8.3: 7 % short */
        K.at = rr; clock_frame();
        if (f > 240){ double l = fabs(rr + theta - (double)MR32(G_WORLDTIME) * 0.00025); if (l > lag) lag = l; }
    }
    CHECK(lag < 0.003, "clock: our own world 7 %% short of the wall clock, WorldTimer kept to it (at most %.2f ms off after 2 s)", lag * 1e3);
    MR32(G_WORLDTIME) = (uint32_t)((rr + theta - 0.4) * 4000.0 + 0.5); K.at = rr;
    uint32_t hard = K.hard; clock_frame();
    CHECK(K.hard == hard + 1 && fabs(rr + theta - (double)MR32(G_WORLDTIME) * 0.00025) < 0.001, "clock: 400 ms off set at once");
    /* the creator: its own clock on the wall clock from the race's start (its clock is everyone's) */
    MR32(G_NET_HOST) = 1; MR32(G_WORLDTIME) = 4000u; K.wok = 0; K.frac = 0; clock_frame();
    {
        double hr0 = rr, hw0 = (double)MR32(G_WORLDTIME) * 0.00025, acc = 0, h1 = 0, h2 = 0, mx = 0;
        for (int f = 0; f < 900; f++){
            rr += 0.022; acc += 0.022 * 0.97;            /* its world 3 % slow, catch-up steps */
            int steps = 0;
            while (acc >= 1.0 / 60 && steps < 3){ acc -= 1.0 / 60; steps++; }
            for (int k = 0; k < (steps ? steps : 1); k++){ setwall(rr + 0.0001 * k); if (steps) MR32(G_WORLDTIME) += 67u; clock_frame(); }
            double o = ((double)MR32(G_WORLDTIME) * 0.00025 - hw0) - (rr - hr0);
            if (f >= 300 && f < 600) h1 += o / 300; else if (f >= 600) h2 += o / 300;
            if (f >= 300 && fabs(o) > mx) mx = fabs(o);
        }
        CHECK(fabs(h2 - h1) < 0.002 && fabs(h2) < 0.012 && mx < 0.03,
              "clock: the creator's clock on the wall clock, its world 3 %% slow: %.2f then %.2f ms off on average, at most %.1f ms",
              h1 * 1e3, h2 * 1e3, mx * 1e3);
    }
    MR32(RC) = 0; uint32_t w1 = MR32(G_WORLDTIME); rr += 0.08; setwall(rr); clock_frame();
    CHECK(MR32(G_WORLDTIME) == w1, "clock: out of the race the game's own (untouched)");
    MR32(RC) = RC_RACING; MR32(G_NET_HOST) = 0; K.wok = 0;

    /* carried on along its curve: a car on a 100 m circle at 40 m/s, states
     * 33 ms apart, shown 200 ms past the newest */
    MR32(CTL + CTL_CAR) = CAR_A; MR32(CAR_A + CAR_INPUT) = INPUT; MR32(CAR_A + 0x28u) = 0;
    const float R0 = 100.f, V0 = 40.f, om = V0 / R0;
    float t0 = 50.f, q0[4] = { 0, 0, 0, 1 }, wz[3] = { 0, 0, om };
    for (int i = 0; i < 2; i++){
        float t = t0 + (float)i / 30.f, ang = om * t;
        float p[3] = { R0 * sinf(ang), R0 * (1.f - cosf(ang)), 0 }, v[3] = { V0 * cosf(ang), V0 * sinf(ang), 0 };
        float q[4] = { 0, 0, sinf(ang / 2), cosf(ang / 2) };
        put_state(t, p, v, wz, q, 0.25f);
        (void)q0;
    }
    Pred *pp = pred_of(CAR_A, 0);
    float tn = t0 + 1.f / 30.f + 0.2f, ang = om * tn, truth[3] = { R0 * sinf(ang), R0 * (1.f - cosf(ang)), 0 };
    float op[3], ov[3], oq[4];
    pred_at(pp, tn, op, ov, oq);
    const PState *s = &pp->s[1];
    float line[3] = { s->p[0] + s->v[0] * 0.2f, s->p[1] + s->v[1] * 0.2f, 0 };
    float yaw = remainderf(2.f * atan2f(oq[2], oq[3]) - ang, 2.f * (float)M_PI) + ang;
    CHECK(pp && pp->n == 2 && dist3(op, truth) < 0.02f && fabsf(yaw - ang) < 0.01f,
          "predict: 200 ms on along the curve %.3f m off (a straight line %.3f m), heading %.4f rad off",
          dist3(op, truth), dist3(line, truth), yaw - ang);

    /* an older game's state repeats the last velocity and spin: taken from the places and orientations */
    {
        Pred *po = pp; PState keep[2] = { po->s[0], po->s[1] }; int kn = po->n;
        float t1 = po->s[1].t + 0.05f, a1 = om * t1;
        float p1[3] = { R0 * sinf(a1), R0 * (1.f - cosf(a1)), 0 }, q1[4] = { 0, 0, sinf(a1 / 2), cosf(a1 / 2) };
        float vold[3] = { po->s[1].v[0], po->s[1].v[1], po->s[1].v[2] }, wold[3] = { po->s[1].w[0], po->s[1].w[1], po->s[1].w[2] };
        float vtrue[3] = { V0 * cosf(a1 - om * 0.025f), V0 * sinf(a1 - om * 0.025f), 0 };   /* (the mean over the 50 ms) */
        put_state(t1, p1, vold, wold, q1, 0.25f);
        float ev = dist3(po->s[1].v, vtrue), ew = fabsf(po->s[1].w[2] - om);
        CHECK(po->old_states == 1 && ev < 0.05f && ew < 0.01f, "older game: a repeated velocity from the places (%.3f m/s off), spin from the orientations (%.4f rad/s off)", ev, ew);
        po->s[0] = keep[0]; po->s[1] = keep[1]; po->n = kn; po->old_states = 0;
    }
    /* the blend: put through the game's apply, the owner's controls kept */
    MR32(G_WORLDTIME) = (uint32_t)((t0 + 1.f / 30.f + 0.1f) * 4000.f); MRF32(G_WORLDDT) = 0.0083f;
    MR32(CTL + CTL_NPRED) = 3; MR32(CTL + CTL_HIT) = 0;
    blend();
    float bp[3]; rd3(CTL + CTL_BLEND + 0x10u, bp);
    float tw = (float)MR32(G_WORLDTIME) * 0.00025f, a2 = om * tw, tr2[3] = { R0 * sinf(a2), R0 * (1.f - cosf(a2)), 0 };
    CHECK(pp->steps == 1 && dist3(bp, tr2) < 0.02f && MRF32(INPUT + 0x214u) == 0.25f && MRF32(INPUT + 0x20cu) == 0.9f,
          "blend: the car where its owner has it at the step's start (%.3f m), its brake %.2f (not the game's 0.7)", dist3(bp, tr2), MRF32(INPUT + 0x214u));
    /* a stall: no state for STALL_S, none of the game's predictions: on its own, then a glide back */
    MR32(G_WORLDTIME) += (uint32_t)(4000.f * (STALL_S + 0.1f)); MR32(CTL + CTL_NPRED) = 0;
    float mark[3] = { 1.f, 2.f, 3.f };
    for (uint32_t k = 0; k < 3; k++) MRF32(CTL + CTL_BLEND + 0x10u + 4 * k) = mark[k];
    blend();
    rd3(CTL + CTL_BLEND + 0x10u, bp);
    CHECK(pp->own == 1 && dist3(bp, mark) < 1e-6f && MRF32(INPUT + 0x214u) == 0.25f, "stall: on its own physics, its brake %.2f", MRF32(INPUT + 0x214u));
    float tnew = (float)MR32(G_WORLDTIME) * 0.00025f - 0.05f, an = om * tnew;
    float pn[3] = { R0 * sinf(an), R0 * (1.f - cosf(an)), 0 }, vn[3] = { V0 * cosf(an), V0 * sinf(an), 0 }, qn[4] = { 0, 0, sinf(an / 2), cosf(an / 2) };
    put_state(tnew, pn, vn, wz, qn, 0.f);
    float tg[3], tgv[3], tgq[4];
    pred_at(pp, (float)MR32(G_WORLDTIME) * 0.00025f, tg, tgv, tgq);
    for (uint32_t k = 0; k < 3; k++) MRF32(CTL + CTL_BLEND + 0x10u + 4 * k) = tg[k] + (k == 0 ? 6.f : 0.f);   /* it ran 6 m off */
    MR32(CTL + CTL_NPRED) = 1;
    blend();
    float e0 = len3(pp->ep);
    for (int f = 0; f < 60; f++){ MR32(G_WORLDTIME) += 33u; blend(); }
    float e1 = len3(pp->ep);
    for (int f = 0; f < 120; f++){ MR32(G_WORLDTIME) += 33u; blend(); }
    CHECK(pp->glides == 1 && e0 > 5.5f && e0 < 6.f && e1 < 1.f && len3(pp->ep) < 0.02f && pp->snaps == 0,
          "stall: back from 6 m off gliding (%.2f m after the first step, %.2f m after 0.5 s, %.3f m after 1.5 s)", e0, e1, len3(pp->ep));
    /* a hit of this game after its newest state: on its own until a newer state */
    MR32(CTL + CTL_HIT) = MR32(G_WORLDTIME); uint32_t own0 = pp->own_steps;
    MR32(G_WORLDTIME) += 33u; blend();
    CHECK(pp->own_steps == own0 + 1, "hit: on its own after a hit here (%u)", pp->own_steps - own0);
    MR32(CTL + CTL_HIT) = 0;
}

int main(void){
    setenv("NFSU2_INPUT_SCRIPT", "racenet_unit", 1);   /* (the test knobs would be read; none set) */
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

    fidelity();

    printf(g_fail ? "racenet unit tests: FAILED\n" : "racenet unit tests: all passed\n");
    return g_fail;
}
