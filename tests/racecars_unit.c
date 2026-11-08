/* racecars_unit.c -- tests for runtime/racecars.c (the racers' own car states,
 * "N2RS", drawn through free roam's interpolator), without the game.
 *
 *   clang -O1 -g -Wall -Wno-unused-function -ffp-contract=off -Iruntime -o out/racecars_unit tests/racecars_unit.c
 *   out/racecars_unit
 *
 * The game's memory is a 4 GB reservation of our own with only the pages a
 * test uses marked committed; the virtual LAN and racenet.c's list of the
 * racers are stand-ins, the clock a fake one.  Cases (we are slot 1):
 *   wire       our car goes to every racer: N2RS, our slot, the state as it is,
 *              the engine as our game sounds it in its extension
 *   stranger   an N2RS from an address that is not a racer: refused
 *   own-slot   one naming our slot: refused
 *   bound      (an older creator: no word on who drives which slot) a
 *              racer's first slot is its own; another racer naming it, or
 *              the first naming another: refused
 *   rtt        its echo of our stamp, less what it held: the round trip
 *   drawn      its car put where its states carry it to, through the game's apply
 *   ghost      a car far away slides to GHOST_US in the past, a near one to
 *              half the round trip ahead
 *   hit        a hit here: its own physics (the car left where it is) until
 *              a state from after the hit, then drawn again
 *   reset      the race over: no racers, no slots
 *   claimed    the next race, the slots known (racenet_slot_of): a racer
 *              naming another's slot from the race's start refused every
 *              time, that car not drawn from its states
 *   honest     then every racer's own slot taken, each car from its own
 *   old-creator slots not known: the first named, as before
 *   engine     a racer whose states carry its engine sounds with its own
 *              (carsfx.c, at its rpm; its AI engine to be silenced), one
 *              whose states carry none (an older game's) keeps the game's;
 *              and the first stops once its states stop carrying one
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

/* racenet.c's racers and the virtual LAN: what was sent */
static uint32_t g_peers[4]; static int g_npeers;
int racenet_race_peers(uint32_t *ip, int max){ int n = g_npeers < max ? g_npeers : max; memcpy(ip, g_peers, 4u * (size_t)n); return n; }
/* and the slot each drives (the creator's race connections, a joiner's list from it): -2, not known (an older
 * creator's list), unless a case sets them */
static uint32_t g_slot_ip[4]; static int g_slot_of[4], g_nslot;
int racenet_slot_of(uint32_t ip){
    if (!g_nslot) return -2;
    for (int i = 0; i < g_nslot; i++) if (g_slot_ip[i] == ip) return g_slot_of[i];
    return -1;
}
static struct { uint32_t to; int n; uint8_t m[256]; } g_out[16]; static int g_nout;
int vlan_rt_send(uint16_t port, uint32_t ip_h, const void *p, int n){
    (void)port;
    if (g_nout < 16 && n <= 256){ g_out[g_nout].to = ip_h; g_out[g_nout].n = n; memcpy(g_out[g_nout].m, p, (size_t)n); g_nout++; }
    return 0;
}
/* the game's apply as dispatch_indirect calls it: the blend's state, into the car */
static int g_applies, g_drivens;
void dispatch_indirect(CPU *c, uint32_t addr);
#include "../runtime/roam_core.c"
#include "../runtime/racecars.c"
/* roam_game.c's engines and carsfx.c (stand-ins): our engine as our game sounds it, a car's GIN files, what is played */
void roam_game_engine_state(uint32_t car, rc_state *s){ (void)car; s->eng = 4321; s->eng_acc = 200; s->eng_dcl = 10; s->eng_base = 20; }
int roam_game_engine_files(CPU *c, uint32_t ride, char gin[2][32], float *idle, float *most){
    (void)c; (void)ride; snprintf(gin[0], 32, "GIN_A.gin"); snprintf(gin[1], 32, "GIN_B.gin"); *idle = 800.f; *most = 7000.f; return 1;
}
void roam_game_engine_sound(const char (*gin)[32], float idle, float most, const rc_pose *P, carsfx_state *st){
    (void)idle; (void)most; st->gin[0] = gin[0]; st->gin[1] = gin[1]; st->eng = P->eng;
}
void roam_game_aieng_patch(void){}
static int g_sfx_cars; static float g_sfx_eng;
void carsfx_begin(const float *pos, const float *fwd, const float *vel){ (void)pos; (void)fwd; (void)vel; g_sfx_cars = 0; }
void carsfx_car(uint32_t who, const char *name, const float *p, const float *v, const carsfx_state *st){
    (void)who; (void)name; (void)p; (void)v; g_sfx_cars++; g_sfx_eng = st->eng;
}
void carsfx_end(void){}
void dispatch_indirect(CPU *c, uint32_t addr){
    if (addr == FN_APPLY){
        uint32_t b = c->ecx, car = MR32(c->esp + 4u);
        g_applies++;
        for (uint32_t k = 0; k < 3; k++){ MR32(car + CAR_POS + 4u * k) = MR32(b + 0x10u + 4u * k); MR32(car + CAR_VEL + 4u * k) = MR32(b + 0x20u + 4u * k); }
    } else if (addr == FN_DRIVEN) g_drivens++;
    c->esp += 4u;
}

#define W      0x10000000u
#define CAR_A  0x10010000u   /* slot 2's car */
#define CAR_B  0x10012000u   /* slot 3's car */
#define CAR_ME 0x10020000u
#define CTL_A  0x10014000u   /* car A's network controller */
#define PL     0x10030000u
#define CAM    0x10032000u   /* player 1's camera (G_VIEW1 +0x40) */
#define RIDE_A 0x10034000u   /* car A's ride */
#define RC     0x10040000u
#define STACK  0x00130000u
#define IP_A   0x0a4d0002u
#define IP_B   0x0a4d0003u
#define IP_X   0x0a4d0009u
static CPU g_c;
static void rows_id(uint32_t car){
    float r[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    for (uint32_t i = 0; i < 9; i++) MRF32(car + CAR_ROWS + 0x10u * (i / 3) + 4u * (i % 3)) = r[i];
}
static void race_setup(void){
    uint32_t g[] = { G_FLOW, G_GAMEMODE, G_WORLD, G_PLAYER1, G_WORLDDT, G_RACE, G_NETPLAYERS };
    for (unsigned i = 0; i < sizeof g / sizeof g[0]; i++) commit(g[i], 32);
    commit(W, 0x100); commit(CAR_A, 0xa00); commit(CAR_B, 0xa00); commit(CAR_ME, 0xa00); commit(CTL_A, 0x200);
    commit(PL, 16); commit(RC, 0x100); commit(STACK - 0x4000u, 0x8000u);
    commit(G_VIEW1, 0x60); commit(CAM, 0x60); commit(RIDE_A, 0x900);
    MR32(G_VIEW1 + 0x40u) = CAM; MR32(CAR_A + 0x10u) = RIDE_A;
    MR32(G_FLOW) = 6; MR32(G_GAMEMODE) = 0x200;
    MR32(G_WORLD) = W; MR32(W) = CAR_A; MR32(CAR_A) = CAR_B; MR32(CAR_B) = CAR_ME; MR32(CAR_ME) = W;
    MR32(G_PLAYER1) = PL; MR32(PL + 4u) = CAR_ME;
    MR32(G_RACE) = RC; MR32(RC) = RC_RACING;
    MRF32(G_WORLDDT) = 0.016667f;
    MR16(CAR_A + CAR_SLOT) = 2; MR16(CAR_B + CAR_SLOT) = 3; MR16(CAR_ME + CAR_SLOT) = 1;
    rows_id(CAR_A); rows_id(CAR_B); rows_id(CAR_ME);
    MRF32(CAR_ME + CAR_POS) = 100.f; MRF32(CAR_ME + CAR_POS + 4u) = 200.f; MRF32(CAR_ME + CAR_POS + 8u) = 5.f;
    MRF32(CAR_ME + CAR_VEL) = 30.f;
    MR32(CTL_A + CTL_CAR) = CAR_A;
    g_peers[0] = IP_A; g_peers[1] = IP_B; g_npeers = 2;
    g_c.esp = STACK;
}
/* a frame: the world's step of 1/60 s (every car moved on by its velocity, as the game's physics after the blend),
 * then the main loop's hook */
static void frame(void){
    g_fake_ns += 16666667u;
    for (uint32_t car = CAR_A; car; car = car == CAR_A ? CAR_B : car == CAR_B ? CAR_ME : 0)
        for (uint32_t k = 0; k < 3; k++) MRF32(car + CAR_POS + 4u * k) += MRF32(car + CAR_VEL + 4u * k) * 0.016667f;
    racecars_frame(&g_c);
}
/* an N2RS of slot at p moving v (x), its stamp t_us, echoing our stamp echo (held); with g_n2rs_eng its engine at
 * that rpm (the sound's scale) in the extension */
static uint16_t g_n2rs_eng;
static int n2rs(uint8_t *m, int slot, uint32_t t_us, float px, float vx, uint32_t tx, uint32_t echo, uint32_t held){
    rc_state s; memset(&s, 0, sizeof s);
    s.t_us = t_us; s.p[0] = px; s.p[1] = 210.f; s.p[2] = 5.f; s.v[0] = vx; s.q[3] = 1.f; s.throttle = 1.f;
    s.ext = g_n2rs_eng != 0; s.eng = g_n2rs_eng; s.eng_acc = 255;
    rc_w w; rc_w_init(&w, m, 256);
    rc_putb(&w, "N2RS", 4); rc_put8(&w, 1); rc_put8(&w, s.ext); rc_put8(&w, (uint32_t)slot); rc_put8(&w, 1);
    rc_put8(&w, 1); rc_put8(&w, 1); rc_put8(&w, RC_ST_ROAM); rc_put8(&w, 0);
    rc_put32(&w, tx); rc_put32(&w, echo); rc_put32(&w, held);
    rc_state_put(&w, &s, s.ext);
    return (int)w.n;
}
static uint32_t now32(void){ return (uint32_t)(g_fake_ns / 1000u); }
/* what a racer 50 ms of round trip away holds of our stamp tx when its message comes now */
static uint32_t held50(uint32_t tx){ return now32() - tx - 50000u; }
static Rc *rc_ip(uint32_t ip){ for (int i = 0; i < NCARS; i++) if (g_rc[i].ip == ip) return &g_rc[i]; return 0; }
/* the car drawn this step: the game's blend (nothing), then ours */
static int draw(uint32_t car, uint32_t ctl){
    int fed = racecars_pre(car);
    return fed && racecars_step(&g_c, ctl, car);
}

int main(void){
    g_mem = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_mem == MAP_FAILED){ perror("mmap"); return 2; }
    /* the cases below draw the cars where their states say (no physics here to drive them): the test knob off */
    setenv("NFSU2_INPUT_SCRIPT", "racecars_unit", 1); setenv("NFSU2_RACE_DRIVE", "0", 1);
    race_setup();
    uint8_t m[256];

    /* wire */
    frame(); frame();
    int sent_a = 0, sent_b = 0, okm = 1;
    for (int i = 0; i < g_nout; i++){
        sent_a += g_out[i].to == IP_A; sent_b += g_out[i].to == IP_B;
        okm &= !memcmp(g_out[i].m, "N2RS", 4) && g_out[i].m[4] == 1 && g_out[i].m[5] == 1 && g_out[i].m[6] == 1 && g_out[i].m[7] == 1;
    }
    rc_state s; rc_r rd; int dec = -1;
    if (g_nout){ rc_r_init(&rd, g_out[g_nout - 1].m + 24, (uint32_t)g_out[g_nout - 1].n - 24u); dec = rc_state_get(&rd, &s, 1); }
    CHECK(sent_a >= 1 && sent_b >= 1 && okm && dec == RC_OK && fabsf(s.p[0] - MRF32(CAR_ME + CAR_POS)) < 0.01f && fabsf(s.v[0] - 30.f) < 0.5f &&
          fabsf(s.q[3]) > 0.999f && s.eng == 4321 && s.eng_acc == 200, "wire: to A %d, to B %d, header %d, the state %d at x %.2f (car %.2f), "
          "vx %.2f, q.w %.3f, engine %u (pulling %u)", sent_a, sent_b, okm, dec, dec == RC_OK ? (double)s.p[0] : 0., (double)MRF32(CAR_ME + CAR_POS),
          dec == RC_OK ? (double)s.v[0] : 0., dec == RC_OK ? (double)s.q[3] : 0., dec == RC_OK ? s.eng : 0u, dec == RC_OK ? s.eng_acc : 0u);
    uint32_t our_tx = g_nout ? (uint32_t)g_out[g_nout - 1].m[12] | (uint32_t)g_out[g_nout - 1].m[13] << 8 | (uint32_t)g_out[g_nout - 1].m[14] << 16 |
                               (uint32_t)g_out[g_nout - 1].m[15] << 24 : 0;

    /* stranger, own-slot */
    uint32_t ref0 = S.refused;
    racecars_heard(m, n2rs(m, 2, 1000000, 110.f, 30.f, now32(), 0, 0), IP_X);
    CHECK(S.refused == ref0 + 1 && !rc_ip(IP_X), "stranger: refused %u, kept %d", S.refused - ref0, !!rc_ip(IP_X));
    racecars_heard(m, n2rs(m, 1, 1000000, 110.f, 30.f, now32(), 0, 0), IP_A);
    CHECK(S.refused == ref0 + 2 && !rc_ip(IP_A), "own-slot: refused %u", S.refused - ref0);

    /* bound */
    racecars_heard(m, n2rs(m, 2, 1000000, 110.f, 30.f, now32(), 0, 0), IP_A);
    Rc *a = rc_ip(IP_A);
    int took = a && a->slot == 2 && a->mo.n == 1 && a->car == CAR_A;
    racecars_heard(m, n2rs(m, 2, 1000000, 300.f, 0.f, now32(), 0, 0), IP_B);
    int b_refused = !rc_ip(IP_B) || rc_ip(IP_B)->slot != 2;
    racecars_heard(m, n2rs(m, 3, 1016667, 111.f, 30.f, now32(), 0, 0), IP_A);
    int a_kept = a && a->slot == 2 && a->mo.n == 1;
    racecars_heard(m, n2rs(m, 3, 1000000, 400.f, 0.f, now32(), 0, 0), IP_B);
    Rc *b = rc_ip(IP_B);
    CHECK(took && b_refused && a_kept && b && b->slot == 3 && b->car == CAR_B, "bound: A takes slot 2 %d, B naming it refused %d, A naming 3 refused %d, "
          "B takes 3 %d", took, b_refused, a_kept, b && b->slot == 3);

    /* rtt: A echoes our stamp, 60 ms after it went, having held it 10 ms */
    g_fake_ns += 60000000u;
    racecars_heard(m, n2rs(m, 2, 1060000, 111.8f, 30.f, now32(), our_tx, 10000), IP_A);
    CHECK(a && a->rn == 1 && abs(a->rtt[0] - 50000) < 200, "rtt: %d samples, %.1f ms", a ? a->rn : 0, a && a->rn ? a->rtt[0] / 1000.0 : 0.);

    /* drawn: its states at 30 m/s along x, 60 a second; drawn where they carry it */
    for (int k = 1; k <= 30; k++){
        frame();
        racecars_heard(m, n2rs(m, 2, 1060000 + (uint32_t)k * 16667u, 111.8f + 0.5f * (float)k, 30.f, now32(), our_tx, held50(our_tx)), IP_A);
        draw(CAR_A, CTL_A);
    }
    int ap0 = g_applies;
    float bx = MRF32(CTL_A + CTL_BLEND + 0x10u), want = 111.8f + 0.5f * 30.f;
    int drew = draw(CAR_A, CTL_A);
    float vw = 30.f / (float)K.vscale;            /* per our world's second (our stamp clock catches up after the 60 ms above) */
    CHECK(drew && g_applies == ap0 + 1 && fabsf(bx - want) < 3.f && fabsf(MRF32(CAR_A + CAR_VEL) - vw) < 0.3f,
          "drawn: %d, applied %d, x %.2f (newest state's %.2f), vx %.2f (%.2f a world second)", drew, g_applies - ap0, (double)bx, (double)want,
          (double)MRF32(CAR_A + CAR_VEL), (double)vw);

    /* ghost: our car 500 m off: its delay slides to GHOST_US; back near it: to half the round trip ahead */
    MRF32(CAR_ME + CAR_POS + 4u) = 800.f;
    for (int k = 31; k <= 300; k++){
        frame();
        racecars_heard(m, n2rs(m, 2, 1060000 + (uint32_t)k * 16667u, 111.8f + 0.5f * (float)k, 30.f, now32(), our_tx, held50(our_tx)), IP_A);
        draw(CAR_A, CTL_A);
    }
    int64_t far_d = a ? a->delay : 0;
    MRF32(CAR_ME + CAR_POS) = MRF32(CAR_A + CAR_POS); MRF32(CAR_ME + CAR_POS + 4u) = MRF32(CAR_A + CAR_POS + 4u) + 3.f;
    for (int k = 301; k <= 600; k++){
        frame();
        MRF32(CAR_ME + CAR_POS) = MRF32(CAR_A + CAR_POS);
        racecars_heard(m, n2rs(m, 2, 1060000 + (uint32_t)k * 16667u, 111.8f + 0.5f * (float)k, 30.f, now32(), our_tx, held50(our_tx)), IP_A);
        draw(CAR_A, CTL_A);
    }
    int64_t near_d = a ? a->delay : 0;
    CHECK(far_d == GHOST_US && near_d == -25000, "ghost: far %.1f ms (want %.1f), near %.1f ms (want -25.0)", far_d / 1000.0, GHOST_US / 1000.0, near_d / 1000.0);

    /* hit: the game stamps one; the car is left where its physics put it */
    MR32(CTL_A + CTL_HIT) = 12345u;
    MRF32(CAR_A + CAR_POS) += 0.7f;
    float hx = MRF32(CAR_A + CAR_POS);
    ap0 = g_applies;
    int dh = draw(CAR_A, CTL_A);
    int own = a && a->own;
    CHECK(dh && own && MRF32(CAR_A + CAR_POS) == hx && g_applies == ap0, "hit: on its own %d, left at %.2f (%.2f), applied %d", own,
          (double)MRF32(CAR_A + CAR_POS), (double)hx, g_applies - ap0);
    for (int k = 601; k <= 640; k++){
        frame();
        racecars_heard(m, n2rs(m, 2, 1060000 + (uint32_t)k * 16667u, 111.8f + 0.5f * (float)k, 30.f, now32(), our_tx, held50(our_tx)), IP_A);
        draw(CAR_A, CTL_A);
    }
    CHECK(a && !a->own && a->own_hit == 1 && a->glides + a->snaps == 1, "hit-back: own %d, hits %u, glides %u, snaps %u", a ? a->own : -1,
          a ? a->own_hit : 0, a ? a->glides : 0, a ? a->snaps : 0);

    /* driven: the car left where it is (its physics moves it), its velocity toward its states' place: 1 m behind
     * it, moving 30 m/s, it gets about 30 + 1 / DRIVE_TAU */
    O.init = 0; setenv("NFSU2_RACE_DRIVE", "1", 1); opts();
    frame();
    racecars_heard(m, n2rs(m, 2, 1060000 + 641u * 16667u, 111.8f + 0.5f * 641.f, 30.f, now32(), our_tx, held50(our_tx)), IP_A);
    draw(CAR_A, CTL_A);                                  /* (on its target) */
    frame();
    racecars_heard(m, n2rs(m, 2, 1060000 + 642u * 16667u, 111.8f + 0.5f * 642.f, 30.f, now32(), our_tx, held50(our_tx)), IP_A);
    MRF32(CAR_A + CAR_POS) -= 1.f;                       /* the physics left it 1 m short */
    float dx = MRF32(CAR_A + CAR_POS);
    ap0 = g_applies;
    int dd = draw(CAR_A, CTL_A);
    float vput = MRF32(CTL_A + CTL_BLEND + 0x20u), want_v = 30.f / (float)K.vscale + 1.f / DRIVE_TAU;
    CHECK(dd && a && a->driving && g_applies == ap0 + 1 && MRF32(CTL_A + CTL_BLEND + 0x10u) == dx && fabsf(vput - want_v) < 1.5f,
          "driven: %d, put at its own place %d, velocity %.2f m/s (about %.2f)", a ? a->driving : 0, MRF32(CTL_A + CTL_BLEND + 0x10u) == dx,
          (double)vput, (double)want_v);

    /* reset */
    MR32(RC) = 0;
    frame();
    int any = 0;
    for (int i = 0; i < NCARS; i++) any |= g_rc[i].ip != 0;
    CHECK(!any && !racecars_fed(CAR_A), "reset: racers left %d", any);

    /* claimed: the next race, the slots known (the creator's race connections, or its list on a joiner: A drives 2,
     * B 3); B names A's slot from the race's start, every frame for half a second (the 2026-11-05 security audit:
     * first come, it spoke for A's car all race, put it where it liked, and its jumps went into every game's report
     * against A) */
    O.init = 0; setenv("NFSU2_RACE_DRIVE", "0", 1); opts();
    MR32(RC) = RC_RACING;
    frame();
    g_slot_ip[0] = IP_A; g_slot_of[0] = 2; g_slot_ip[1] = IP_B; g_slot_of[1] = 3; g_nslot = 2;
    uint32_t ref1 = S.refused;
    for (int k = 0; k < 30; k++){
        frame();
        racecars_heard(m, n2rs(m, 2, 3000000 + (uint32_t)k * 16667u, 500.f, 0.f, now32(), 0, 0), IP_B);
    }
    uint32_t tries = S.refused - ref1;
    CHECK(tries == 30 && !rc_ip(IP_B) && !racecars_fed(CAR_A), "claimed: B naming A's slot 2 from the start refused %u of 30, "
          "A's car from B's states %d", tries, racecars_fed(CAR_A));

    /* honest: then A's states for slot 2 and B's for slot 3 (B still naming 2 now and then): each car from its own */
    for (int k = 0; k < 10; k++){
        frame();
        uint32_t t = 3500000 + (uint32_t)k * 16667u;
        racecars_heard(m, n2rs(m, 2, t, 120.f + 0.5f * (float)k, 30.f, now32(), 0, 0), IP_A);
        racecars_heard(m, n2rs(m, 3, t, 400.f, 0.f, now32(), 0, 0), IP_B);
        if (k % 3 == 0) racecars_heard(m, n2rs(m, 2, t, 500.f, 0.f, now32(), 0, 0), IP_B);
    }
    Rc *a2 = rc_ip(IP_A), *b2 = rc_ip(IP_B);
    CHECK(a2 && a2->slot == 2 && a2->n == 10 && rc_of_car(CAR_A) == a2 && b2 && b2->slot == 3 && b2->n == 10 && rc_of_car(CAR_B) == b2 &&
          S.refused == ref1 + 34, "honest: A for slot 2 %d (%u states), B for slot 3 %d (%u states), B naming 2 refused %u more",
          a2 && a2->slot == 2 && rc_of_car(CAR_A) == a2, a2 ? a2->n : 0, b2 && b2->slot == 3 && rc_of_car(CAR_B) == b2, b2 ? b2->n : 0,
          S.refused - ref1 - 30);

    /* old-creator: the next race, the slots not known (a list from a creator of an older version): the first slot an
     * address names, as before (not every state refused) */
    MR32(RC) = 0; frame(); MR32(RC) = RC_RACING; frame();
    g_nslot = 0;
    racecars_heard(m, n2rs(m, 2, 4000000, 500.f, 0.f, now32(), 0, 0), IP_B);
    racecars_heard(m, n2rs(m, 2, 4000000, 120.f, 30.f, now32(), 0, 0), IP_A);
    racecars_heard(m, n2rs(m, 3, 4000000, 400.f, 0.f, now32(), 0, 0), IP_A);
    Rc *a3 = rc_ip(IP_A), *b3 = rc_ip(IP_B);
    CHECK(b3 && b3->slot == 2 && a3 && a3->slot == 3, "old-creator: B naming slot 2 first takes it %d, A then slot 3 %d",
          b3 && b3->slot == 2, a3 && a3->slot == 3);

    /* engine: the next race; A's states carry its engine, B's none (an older game's) */
    MR32(RC) = 0; frame(); MR32(RC) = RC_RACING; frame();
    for (int k = 0; k < 20; k++){
        frame();
        uint32_t t = 5000000 + (uint32_t)k * 16667u;
        g_n2rs_eng = (uint16_t)(5000 + 10 * k); racecars_heard(m, n2rs(m, 2, t, 120.f + 0.5f * (float)k, 30.f, now32(), 0, 0), IP_A);
        g_n2rs_eng = 0; racecars_heard(m, n2rs(m, 3, t, 400.f, 0.f, now32(), 0, 0), IP_B);
        draw(CAR_A, CTL_A); draw(CAR_B, 0);
    }
    frame();
    float ha = racecars_engine_heard(CAR_A), hb = racecars_engine_heard(CAR_B);
    CHECK(g_sfx_cars == 1 && ha > 5100.f && ha < 5200.f && fabsf(g_sfx_eng - ha) < 0.5f && hb == 0.f,
          "engine: %d car(s) played, A's at %.0f (its last states 5100 .. 5190), carsfx.c given %.0f; B's %.0f (none)", g_sfx_cars,
          (double)ha, (double)g_sfx_eng, (double)hb);
    /* ...then A's states without one (a game of before put on mid-race, say): back to the game's */
    for (int k = 20; k < 40; k++){
        frame();
        racecars_heard(m, n2rs(m, 2, 5000000 + (uint32_t)k * 16667u, 120.f + 0.5f * (float)k, 30.f, now32(), 0, 0), IP_A);
        draw(CAR_A, CTL_A);
    }
    frame();
    CHECK(g_sfx_cars == 0 && racecars_engine_heard(CAR_A) == 0.f, "engine-off: %d car(s) played, A's %.0f", g_sfx_cars,
          (double)racecars_engine_heard(CAR_A));

    printf("racecars unit tests: %s\n", g_fail ? "FAILED" : "all passed");
    return g_fail;
}
