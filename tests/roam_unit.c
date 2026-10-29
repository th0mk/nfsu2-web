/* roam_unit.c -- unit tests for the free-roam multiplayer core (roam_core.c)
 * and the platform layer's pure parts (roam_bsd.c: guard, broadcast
 * targets, peer list).  No socket is opened: sessions talk through an
 * in-process fake network with a fake clock.
 *
 *   clang -O1 -g -fsanitize=address,undefined -Iruntime -o out/roam_unit \
 *         tests/roam_unit.c runtime/roam_core.c runtime/roam_bsd.c
 *   out/roam_unit [fuzz-iterations]
 */
#include "roam_core.h"
#include "roam_plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do{ if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } }while(0)

/* roam_bsd.c logs events through the game; here they are counted */
static int g_ev[8];
void roam_game_event(int ev, int peer, const char *text){ (void)peer; (void)text; if (ev >= 0 && ev < 8) g_ev[ev]++; }
int roam_game_racing(void){ return 0; }      /* roam_web.c (the wasm build of these tests) */
void gw_marks(const void *m, int n){ (void)m; (void)n; }      /* roam_web.c: the renderer's frame (gpu_web.c) */

static uint32_t rng = 12345;
static uint32_t rnd(void){ rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static float frnd(float lo, float hi){ return lo + (hi - lo) * (float)(rnd() % 1000000u) / 1e6f; }

/* ---- 1. byte layout against the documented offsets -------------------------------- */
static uint32_t le32(const uint8_t *p){ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p){ return (uint16_t)(p[0] | p[1] << 8); }
static float lef(const uint8_t *p){ uint32_t u = le32(p); float f; memcpy(&f, &u, 4); return f; }
static void t_layout(void){
    uint8_t b[RC_MAX_DGRAM];
    rc_msg m; memset(&m, 0, sizeof m);
    m.h.type = RC_MSG_HELLO; m.h.sender = 0xA1B2C3D4u; m.h.seq = 0x1234; m.h.flags = 0;
    m.u.hello.core_ver = 1; m.u.hello.platform = RC_PLAT_MAC; m.u.hello.status = RC_ST_ROAM; m.u.hello.listen_port = 47210;
    m.u.hello.cfg_rev = 7; m.u.hello.room = 0x01020304u; strcpy(m.u.hello.name, "Thom"); m.u.hello.n_seen = 2;
    m.u.hello.seen[0] = 0x11111111u; m.u.hello.seen[1] = 0x22222222u;
    int n = rc_encode(&m, b, sizeof b);
    /* header 16 + core_ver 2 + platform 1 + status 1 + port 2 + rev 2 + room 4 + len 1 + name 4 + n_seen 1 + 8 + crc 4 */
    CHECK(n == 16 + 2 + 1 + 1 + 2 + 2 + 4 + 1 + 4 + 1 + 8 + 4, "HELLO length %d", n);
    CHECK(!memcmp(b, "N2FR", 4) && b[4] == 1 && b[5] == RC_MSG_HELLO, "HELLO magic/version/type");
    CHECK(le16(b + 6) == n && le32(b + 8) == 0xA1B2C3D4u && le16(b + 12) == 0x1234 && le16(b + 14) == 0, "HELLO header fields");
    CHECK(le16(b + 16) == 1 && b[18] == RC_PLAT_MAC && b[19] == RC_ST_ROAM && le16(b + 20) == 47210 && le16(b + 22) == 7 &&
          le32(b + 24) == 0x01020304u && b[28] == 4 && !memcmp(b + 29, "Thom", 4) && b[33] == 2 &&
          le32(b + 34) == 0x11111111u && le32(b + 38) == 0x22222222u, "HELLO payload offsets");
    CHECK(le32(b + n - 4) == rc_crc32(b, (uint32_t)n - 4), "HELLO CRC trailer");

    memset(&m, 0, sizeof m);
    m.h.type = RC_MSG_SNAP; m.h.sender = 5; m.h.seq = 9;
    m.u.snap.status = RC_ST_ROAM; m.u.snap.epoch = 3; m.u.snap.cfg_rev = 0x0102; m.u.snap.n = 1; m.u.snap.clock = 4;
    rc_state *s = &m.u.snap.s[0];
    s->t_us = 0xDEADBEEFu; s->p[0] = 1.5f; s->p[1] = -2.25f; s->p[2] = 3.0f; s->v[0] = 10; s->v[1] = 0; s->v[2] = -1;
    s->q[0] = 0; s->q[1] = 0; s->q[2] = 0; s->q[3] = 1; s->w[0] = 0.5f; s->w[1] = -0.25f; s->w[2] = 1.0f;
    s->steer = -0.5f; s->throttle = 1.0f; s->brake = 0.0f; s->rpm = 4500; s->speed = 33.33f; s->gear = 3; s->bits = RC_BIT_BRAKE;
    n = rc_encode(&m, b, sizeof b);
    CHECK(n == 16 + 6 + 52 + 4, "SNAP length %d", n);
    const uint8_t *st = b + 22;
    CHECK(b[16] == RC_ST_ROAM && b[17] == 3 && le16(b + 18) == 0x0102 && b[20] == 1 && b[21] == 4, "SNAP header payload");
    CHECK(le32(st) == 0xDEADBEEFu && lef(st + 4) == 1.5f && lef(st + 8) == -2.25f && lef(st + 12) == 3.0f &&
          lef(st + 16) == 10.f && lef(st + 24) == -1.f, "SNAP state floats");
    CHECK((int16_t)le16(st + 28) == 0 && (int16_t)le16(st + 34) == 32767, "SNAP quaternion i16 (w = 32767)");
    CHECK((int16_t)le16(st + 36) == 500 && (int16_t)le16(st + 38) == -250 && (int16_t)le16(st + 40) == 1000, "SNAP angvel x1000");
    CHECK((int16_t)le16(st + 42) == -16384 && st[44] == 255 && st[45] == 0 && le16(st + 46) == 4500 &&
          (int16_t)le16(st + 48) == 3333 && (int8_t)st[50] == 3 && st[51] == RC_BIT_BRAKE, "SNAP controls");
    /* every state bit goes through, the passable and busy ones too (roam_game.c; an older core ignores them) */
    s->bits = RC_BIT_PASS | RC_BIT_BUSY | RC_BIT_HORN;
    n = rc_encode(&m, b, sizeof b);
    rc_msg sd; memset(&sd, 0, sizeof sd);
    CHECK(n == 16 + 6 + 52 + 4 && st[51] == (RC_BIT_PASS | RC_BIT_BUSY | RC_BIT_HORN) && rc_decode(b, n, &sd) == RC_OK &&
          sd.u.snap.s[0].bits == (RC_BIT_PASS | RC_BIT_BUSY | RC_BIT_HORN), "SNAP state bits");

    memset(&m, 0, sizeof m);
    m.h.type = RC_MSG_PEERS; m.h.sender = 5; m.u.peers.n = 1; m.u.peers.e[0].ip = 0xC0A80114u; m.u.peers.e[0].port = 47211; m.u.peers.e[0].id = 0x55667788u;
    n = rc_encode(&m, b, sizeof b);
    CHECK(n == 16 + 1 + 10 + 4 && b[16] == 1 && b[17] == 192 && b[18] == 168 && b[19] == 1 && b[20] == 20 &&
          le16(b + 21) == 47211 && le32(b + 23) == 0x55667788u, "PEERS layout");

    uint8_t rec[1500]; for (int i = 0; i < 1500; i++) rec[i] = (uint8_t)i;
    memset(&m, 0, sizeof m);
    m.h.type = RC_MSG_CFG; m.h.sender = 5; m.u.cfg.cfg_rev = 2; m.u.cfg.frag = 1; m.u.cfg.nfrag = 2; m.u.cfg.total = 1500;
    m.u.cfg.dlen = 500; m.u.cfg.data = rec + 1000;
    n = rc_encode(&m, b, sizeof b);
    CHECK(n == 16 + 8 + 500 + 4 && le16(b + 16) == 2 && b[18] == 1 && b[19] == 2 && le16(b + 20) == 1500 && le16(b + 22) == 500 &&
          b[24] == (uint8_t)1000, "CFG layout");

    memset(&m, 0, sizeof m); m.h.type = RC_MSG_PONG; m.h.sender = 5; m.u.ping.t0 = 1; m.u.ping.t1 = 2; m.u.ping.t2 = 3;
    n = rc_encode(&m, b, sizeof b);
    CHECK(n == 16 + 12 + 4 && le32(b + 16) == 1 && le32(b + 20) == 2 && le32(b + 24) == 3, "PONG layout");
    memset(&m, 0, sizeof m); m.h.type = RC_MSG_BYE; m.h.sender = 5; m.u.bye.reason = 1;
    n = rc_encode(&m, b, sizeof b);
    CHECK(n == 16 + 1 + 4 && b[16] == 1, "BYE layout");
    memset(&m, 0, sizeof m); m.h.type = RC_MSG_CFG_REQ; m.h.sender = 5; m.u.cfgreq.cfg_rev = 9; m.u.cfgreq.have_mask = 1;
    n = rc_encode(&m, b, sizeof b);
    CHECK(n == 16 + 3 + 4 && le16(b + 16) == 9 && b[18] == 1, "CFG_REQ layout");

    /* CRC-32 check value */
    CHECK(rc_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u, "CRC-32 check value");
    /* the room's hash: capitals and spaces do not count (the start page's roomHash has the same value) */
    CHECK(rc_room_hash("blueroom") == 0x160FBA32u && rc_room_hash(" Blue Room\t") == 0x160FBA32u && rc_fnv1a("blueroom") == 0x160FBA32u,
          "room hash %08x", rc_room_hash(" Blue Room\t"));
    CHECK(rc_room_hash("") == 0 && rc_room_hash("  ") == 0 && rc_room_hash(0) == 0 && rc_room_hash("Zo\xc3\xab") != rc_room_hash("zo\xc3\x8b"),
          "no room for spaces only; only A-Z made small");
    /* a fixed message: the exact bytes (regression of the format) */
    memset(&m, 0, sizeof m); m.h.type = RC_MSG_PING; m.h.sender = 0x01020304u; m.h.seq = 1; m.u.ping.t0 = 0x0A0B0C0Du;
    n = rc_encode(&m, b, sizeof b);
    static const uint8_t ping_gold[24] = { 0x4E,0x32,0x46,0x52, 0x01, 0x06, 0x18,0x00, 0x04,0x03,0x02,0x01, 0x01,0x00, 0x00,0x00,
                                           0x0D,0x0C,0x0B,0x0A, 0,0,0,0 };
    uint8_t g[24]; memcpy(g, ping_gold, 24); uint32_t c = rc_crc32(g, 20); g[20] = (uint8_t)c; g[21] = (uint8_t)(c >> 8); g[22] = (uint8_t)(c >> 16); g[23] = (uint8_t)(c >> 24);
    CHECK(n == 24 && !memcmp(b, g, 24), "PING golden bytes");
    printf("  PING datagram: "); for (int i = 0; i < n; i++) printf("%02x", b[i]); printf("\n");
}

/* ---- 2. round trips ------------------------------------------------------------------------ */
static void t_roundtrip(void){
    uint8_t b[RC_MAX_DGRAM]; rc_msg m, d;
    for (int it = 0; it < 2000; it++){
        memset(&m, 0, sizeof m);
        m.h.type = RC_MSG_SNAP; m.h.sender = rnd() | 1u; m.h.seq = (uint16_t)rnd();
        m.u.snap.status = (uint8_t)(rnd() % RC_ST__N); m.u.snap.epoch = (uint8_t)rnd(); m.u.snap.clock = (uint8_t)rnd();
        m.u.snap.cfg_rev = (uint16_t)rnd(); m.u.snap.n = (uint8_t)(1 + rnd() % 3);
        for (int k = 0; k < m.u.snap.n; k++){
            rc_state *s = &m.u.snap.s[k];
            s->t_us = rnd();
            for (int i = 0; i < 3; i++){ s->p[i] = frnd(-5000, 5000); s->v[i] = frnd(-60, 60); s->w[i] = frnd(-5, 5); }
            s->p[2] = frnd(-500, 500);
            float q[4] = { frnd(-1, 1), frnd(-1, 1), frnd(-1, 1), frnd(-1, 1) }, nn = sqrtf(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
            for (int i = 0; i < 4; i++) s->q[i] = q[i] / nn;
            s->steer = frnd(-1, 1); s->throttle = frnd(0, 1); s->brake = frnd(0, 1); s->rpm = frnd(0, 9000); s->speed = frnd(-20, 90);
            s->gear = (int8_t)(rnd() % 8); s->bits = (uint8_t)rnd();
        }
        int n = rc_encode(&m, b, sizeof b);
        int e = rc_decode(b, n, &d);
        if (e){ CHECK(0, "snap decode %d", e); break; }
        int okk = d.h.sender == m.h.sender && d.h.seq == m.h.seq && d.u.snap.n == m.u.snap.n && d.u.snap.epoch == m.u.snap.epoch &&
                  d.u.snap.clock == m.u.snap.clock && d.u.snap.cfg_rev == m.u.snap.cfg_rev && d.u.snap.status == m.u.snap.status;
        for (int k = 0; k < m.u.snap.n && okk; k++){
            const rc_state *a = &m.u.snap.s[k], *z = &d.u.snap.s[k];
            okk = a->t_us == z->t_us && a->gear == z->gear && a->bits == z->bits;
            for (int i = 0; i < 3; i++) okk = okk && a->p[i] == z->p[i] && a->v[i] == z->v[i] && fabsf(a->w[i] - z->w[i]) <= 0.0006f;
            float dot = 0; for (int i = 0; i < 4; i++) dot += a->q[i] * z->q[i];
            okk = okk && fabsf(dot) > 0.99999f && fabsf(a->steer - z->steer) < 1e-4f && fabsf(a->throttle - z->throttle) < 0.003f &&
                  fabsf(a->rpm - z->rpm) <= 0.5f && fabsf(a->speed - z->speed) <= 0.0051f;
        }
        if (!okk){ CHECK(0, "snap round trip %d", it); break; }
    }
    g_pass++;
    /* HELLO with every name length, PEERS with every count */
    for (int nl = 0; nl <= RC_NAME_MAX; nl++){
        memset(&m, 0, sizeof m); m.h.type = RC_MSG_HELLO; m.h.sender = 7;
        for (int i = 0; i < nl; i++) m.u.hello.name[i] = (char)('a' + i % 26);
        m.u.hello.n_seen = (uint8_t)(nl % (RC_MAX_PEERS + 1));
        for (int i = 0; i < m.u.hello.n_seen; i++) m.u.hello.seen[i] = rnd();
        int n = rc_encode(&m, b, sizeof b);
        CHECK(n > 0 && rc_decode(b, n, &d) == RC_OK && !strcmp(d.u.hello.name, m.u.hello.name) && d.u.hello.n_seen == m.u.hello.n_seen &&
              !memcmp(d.u.hello.seen, m.u.hello.seen, 4u * m.u.hello.n_seen), "HELLO round trip, name length %d", nl);
    }
    memset(&m, 0, sizeof m); m.h.type = RC_MSG_HELLO; m.h.sender = 7; memset(m.u.hello.name, 'x', RC_NAME_MAX + 1); m.u.hello.name[RC_NAME_MAX] = 'y';
    CHECK(rc_encode(&m, b, sizeof b) < 0, "HELLO name longer than 24 refused");
    /* a name heard that reads as a slur (names.h) comes as "Player", another as it is */
    memset(&m, 0, sizeof m); m.h.type = RC_MSG_HELLO; m.h.sender = 7; strcpy(m.u.hello.name, "N1-gg 4");
    { int n = rc_encode(&m, b, sizeof b);
      CHECK(n > 0 && rc_decode(b, n, &d) == RC_OK && !strcmp(d.u.hello.name, "Player"), "HELLO: a slur heard is Player ('%s')", d.u.hello.name); }
    strcpy(m.u.hello.name, "Mustafa GS");
    { int n = rc_encode(&m, b, sizeof b);
      CHECK(n > 0 && rc_decode(b, n, &d) == RC_OK && !strcmp(d.u.hello.name, "Mustafa GS"), "HELLO: another name as it is ('%s')", d.u.hello.name); }
    /* DUEL: 10 bytes of payload; no op, an unknown op, no player or no id refused */
    memset(&m, 0, sizeof m); m.h.type = RC_MSG_DUEL; m.h.sender = 7;
    m.u.duel.op = RC_DUEL_RESULT; m.u.duel.arg = 2; m.u.duel.to = 0xCAFEF00Du; m.u.duel.duel = 0x12345678u;
    { int n = rc_encode(&m, b, sizeof b);
      CHECK(n == RC_HDR + 10 + RC_CRC && rc_decode(b, n, &d) == RC_OK && d.u.duel.op == RC_DUEL_RESULT && d.u.duel.arg == 2 &&
            d.u.duel.to == 0xCAFEF00Du && d.u.duel.duel == 0x12345678u, "DUEL round trip (%d bytes)", n);
      static const struct { int at; uint8_t v; } bad[] = { { RC_HDR, 0 }, { RC_HDR, RC_DUEL__N }, { RC_HDR + 2, 0 }, { RC_HDR + 6, 0 } };
      for (int k = 0; k < 4; k++){
          uint8_t c[RC_MAX_DGRAM]; memcpy(c, b, (size_t)n);
          c[bad[k].at] = bad[k].v;
          if (bad[k].at >= RC_HDR + 2) for (int j = 1; j < 4; j++) c[bad[k].at + j] = 0;
          uint32_t crc = rc_crc32(c, (uint32_t)n - RC_CRC); for (int j = 0; j < 4; j++) c[n - 4 + j] = (uint8_t)(crc >> (8 * j));
          CHECK(rc_decode(c, n, &d) == RC_E_FIELD, "DUEL with a bad field refused (%d)", k);
      }
      m.u.duel.op = 0; CHECK(rc_encode(&m, b, sizeof b) < 0, "DUEL without an op not encoded"); }
    /* LOBBY: 24 bytes and 4 per player; a bad op, state, reason, id, race, size or player list refused */
    memset(&m, 0, sizeof m); m.h.type = RC_MSG_LOBBY; m.h.sender = 7;
    { rc_lobby *l = &m.u.lobby;
      l->op = RC_LOBBY_ADVERT; l->arg = 0; l->state = RC_LS_STARTING; l->max = 6; l->to = 0xCAFEF00Du; l->lobby = 0x1234ABCDu;
      l->race = 0x9E3779B9u; l->x = -1234; l->y = 4321; l->t_ms = 5000; l->n = 3; l->away_s = 17; l->ids[0] = 7; l->ids[1] = 0x51; l->ids[2] = 0x52;
      int n = rc_encode(&m, b, sizeof b);
      const rc_lobby *e = &d.u.lobby;
      CHECK(n == RC_HDR + 24 + 12 + RC_CRC && rc_decode(b, n, &d) == RC_OK && e->op == RC_LOBBY_ADVERT && e->state == RC_LS_STARTING &&
            e->max == 6 && e->to == 0xCAFEF00Du && e->lobby == 0x1234ABCDu && e->race == 0x9E3779B9u && e->x == -1234 && e->y == 4321 &&
            e->t_ms == 5000 && e->n == 3 && e->away_s == 17 && e->ids[0] == 7 && e->ids[2] == 0x52, "LOBBY round trip (%d bytes)", n);
      /* the away byte where the reserved one was (after n): what older games and the hub skip */
      CHECK(b[RC_HDR + 22] == 3 && b[RC_HDR + 23] == 17, "LOBBY away byte after n");
      /* op 0, op past the last, reason past the last, state past the last, max 1, max 7, max 2 (< 3 players),
         lobby 0, race 0, the second player the same as the first, the third player 0 */
      static const struct { int at; uint8_t v; int zero3; } bad[] = { { RC_HDR, 0, 0 }, { RC_HDR, RC_LOBBY__N, 0 }, { RC_HDR + 1, RC_LL__N, 0 },
          { RC_HDR + 2, RC_LS__N, 0 }, { RC_HDR + 3, 1, 0 }, { RC_HDR + 3, RC_LOBBY_MAX + 1, 0 }, { RC_HDR + 3, 2, 0 }, { RC_HDR + 8, 0, 1 },
          { RC_HDR + 12, 0, 1 }, { RC_HDR + 24 + 4, 7, 1 }, { RC_HDR + 24 + 8, 0, 1 } };
      int nb = (int)(sizeof bad / sizeof bad[0]);
      for (int k = 0; k < nb; k++){
          uint8_t c[RC_MAX_DGRAM]; memcpy(c, b, (size_t)n);
          c[bad[k].at] = bad[k].v;
          if (bad[k].zero3) for (int j = 1; j < 4; j++) c[bad[k].at + j] = 0;
          uint32_t crc = rc_crc32(c, (uint32_t)n - RC_CRC); for (int j = 0; j < 4; j++) c[n - 4 + j] = (uint8_t)(crc >> (8 * j));
          CHECK(rc_decode(c, n, &d) == RC_E_FIELD, "LOBBY with a bad field refused (%d)", k);
      }
      l->n = 7; CHECK(rc_encode(&m, b, sizeof b) < 0, "LOBBY with more players than it takes not encoded");
      l->n = 0; l->lobby = 0; CHECK(rc_encode(&m, b, sizeof b) < 0, "LOBBY without an id not encoded"); }
    /* LOBBIES: 3 bytes and RC_LOBBIES_ENTRY per lobby */
    memset(&m, 0, sizeof m); m.h.type = RC_MSG_LOBBIES; m.h.sender = 7;
    { static uint8_t le[2 * RC_LOBBIES_ENTRY] = { 1, 0, 2, 0, 0x11, 0x22, 0x33, 0x44, 2, 6, 0,  0xff, 0xff, 0, 1, 1, 0, 0, 0, 4, 4, 1 };
      m.u.lobbies.req = 99; m.u.lobbies.n = 2; m.u.lobbies.data = le;
      int n = rc_encode(&m, b, sizeof b);
      CHECK(n == RC_HDR + 3 + 2 * RC_LOBBIES_ENTRY + RC_CRC && rc_decode(b, n, &d) == RC_OK && d.u.lobbies.req == 99 && d.u.lobbies.n == 2 &&
            !memcmp(d.u.lobbies.data, le, sizeof le), "LOBBIES round trip (%d bytes)", n);
      m.u.lobbies.n = RC_LOBBIES_MAX + 1; CHECK(rc_encode(&m, b, sizeof b) < 0, "LOBBIES of too many not encoded"); }
    /* SAY: 4 bytes (phrase, seq, two zero); any phrase number goes through (a newer game may have more) */
    memset(&m, 0, sizeof m); m.h.type = RC_MSG_SAY; m.h.sender = 7; m.u.say.phrase = 13; m.u.say.seq = 200;
    { int n = rc_encode(&m, b, sizeof b);
      CHECK(n == RC_HDR + 4 + RC_CRC && b[RC_HDR] == 13 && b[RC_HDR + 1] == 200 && !b[RC_HDR + 2] && !b[RC_HDR + 3] &&
            rc_decode(b, n, &d) == RC_OK && d.u.say.phrase == 13 && d.u.say.seq == 200, "SAY round trip (%d bytes)", n);
      m.u.say.phrase = 250; n = rc_encode(&m, b, sizeof b);
      CHECK(rc_decode(b, n, &d) == RC_OK && d.u.say.phrase == 250, "SAY with a phrase this game does not have still decodes");
      CHECK(rc_decode(b, n - 1, &d) != RC_OK, "SAY cut short refused"); }
    /* RACEREP: RC_RACEREP_BYTES; no lobby, no race, no cars, a place past them, an unknown flag, a class past 3, a
       number out of range or not one, an unprintable name refused */
    memset(&m, 0, sizeof m); m.h.type = RC_MSG_RACEREP; m.h.sender = 7;
    { rc_racerep *r = &m.u.racerep;
      r->lobby = 0x1234ABCDu; r->race = 0x9E3779B9u; r->mode = 10; r->laps = 4; r->track = 4302; r->place = 3; r->players = 6;
      r->flags = RC_RR_QUIT; r->type = 27; r->cls = -1; r->rating = 591.1f; r->field = 450.25f; r->factor = 0.75f; r->kw = 368.f;
      r->kg = 1338.f; r->kmh = 343.f; r->time = 61.25f; strcpy(r->name, "SKYLINE");
      int n = rc_encode(&m, b, sizeof b);
      const rc_racerep *e = &d.u.racerep;
      CHECK(n == RC_HDR + RC_RACEREP_BYTES + RC_CRC && rc_decode(b, n, &d) == RC_OK && e->lobby == 0x1234ABCDu && e->race == 0x9E3779B9u &&
            e->mode == 10 && e->laps == 4 && e->track == 4302 && e->place == 3 && e->players == 6 && e->flags == RC_RR_QUIT && e->type == 27 &&
            e->cls == -1 && e->rating == 591.1f && e->field == 450.25f && e->factor == 0.75f && e->kw == 368.f && e->kg == 1338.f &&
            e->kmh == 343.f && e->time == 61.25f && !strcmp(e->name, "SKYLINE"), "RACEREP round trip (%d bytes)", n);
      static const struct { int at; uint8_t v; int zero3; } bad[] = { { RC_HDR, 0, 1 }, { RC_HDR + 4, 0, 1 }, { RC_HDR + 13, 0, 0 },
          { RC_HDR + 12, 7, 0 }, { RC_HDR + 14, 4, 0 }, { RC_HDR + 16, 4, 0 }, { RC_HDR + 23, 0x7F, 0 }, { RC_HDR + 31, 0xFF, 0 },
          { RC_HDR + 48, 0x07, 0 } };
      int nb = (int)(sizeof bad / sizeof bad[0]);
      for (int k = 0; k < nb; k++){
          uint8_t c[RC_MAX_DGRAM]; memcpy(c, b, (size_t)n);
          c[bad[k].at] = bad[k].v;
          if (bad[k].zero3) for (int j = 1; j < 4; j++) c[bad[k].at + j] = 0;
          uint32_t crc = rc_crc32(c, (uint32_t)n - RC_CRC); for (int j = 0; j < 4; j++) c[n - 4 + j] = (uint8_t)(crc >> (8 * j));
          CHECK(rc_decode(c, n, &d) == RC_E_FIELD, "RACEREP with a bad field refused (%d)", k);
      }
      r->place = 7; CHECK(rc_encode(&m, b, sizeof b) < 0, "RACEREP with a place past its cars not encoded"); }
    /* RACERES: RC_RACERES_HEAD + n rows of RC_RACERES_ROW; no lobby, no race, no rows or more than RC_LOBBY_MAX, an
       unknown flag, two rows ours, a time or a distance out of range refused, a byte more refused */
    memset(&m, 0, sizeof m); m.h.type = RC_MSG_RACERES; m.h.sender = 7;
    { rc_raceres *r = &m.u.raceres;
      r->lobby = 0x1234ABCDu; r->race = 0x9E3779B9u; r->mode = 1; r->laps = 2; r->n = 3;
      r->row[0].id = 7; r->row[0].flags = RC_RS_FINISHED | RC_RS_OURS; r->row[0].time = 21.5f; r->row[0].done = 2.f; r->row[0].dist = 5800.f;
      r->row[1].id = 8; r->row[1].flags = 0; r->row[1].done = 0.61f; r->row[1].dist = 1750.5f;
      r->row[2].id = 0; r->row[2].flags = 0; r->row[2].done = 0.f;
      int n = rc_encode(&m, b, sizeof b);
      const rc_raceres *e = &d.u.raceres;
      CHECK(n == RC_HDR + RC_RACERES_HEAD + 3 * RC_RACERES_ROW + RC_CRC && rc_decode(b, n, &d) == RC_OK && e->lobby == 0x1234ABCDu &&
            e->race == 0x9E3779B9u && e->mode == 1 && e->laps == 2 && e->n == 3 && e->row[0].id == 7 &&
            e->row[0].flags == (RC_RS_FINISHED | RC_RS_OURS) && e->row[0].time == 21.5f && e->row[0].done == 2.f &&
            e->row[0].dist == 5800.f &&
            e->row[1].id == 8 && !e->row[1].flags && e->row[1].done == 0.61f && e->row[1].dist == 1750.5f && !e->row[2].id,
            "RACERES round trip (%d bytes)", n);
      static const struct { int at; uint8_t v; int zero3; } bad[] = { { RC_HDR, 0, 1 }, { RC_HDR + 4, 0, 1 }, { RC_HDR + 10, 0, 0 },
          { RC_HDR + 10, 7, 0 }, { RC_HDR + 16, 4, 0 }, { RC_HDR + 36, 2, 0 }, { RC_HDR + 23, 0xFF, 0 }, { RC_HDR + 27, 0x7F, 0 },
          { RC_HDR + 31, 0xFF, 0 } };
      int nb = (int)(sizeof bad / sizeof bad[0]);
      for (int k = 0; k < nb; k++){
          uint8_t c[RC_MAX_DGRAM]; memcpy(c, b, (size_t)n);
          c[bad[k].at] = bad[k].v;
          if (bad[k].zero3) for (int j = 1; j < 4; j++) c[bad[k].at + j] = 0;
          uint32_t crc = rc_crc32(c, (uint32_t)n - RC_CRC); for (int j = 0; j < 4; j++) c[n - 4 + j] = (uint8_t)(crc >> (8 * j));
          int got = rc_decode(c, n, &d);
          CHECK(got == RC_E_FIELD || got == RC_E_SHORT || got == RC_E_TRAIL, "RACERES with a bad field refused (%d: %d)", k, got);
      }
      r->n = 0; CHECK(rc_encode(&m, b, sizeof b) < 0, "RACERES of no rows not encoded");
      r->n = 3; r->row[1].flags = RC_RS_OURS; CHECK(rc_encode(&m, b, sizeof b) < 0, "RACERES with two rows ours not encoded"); }
    /* car record */
    rc_carcfg cc, c2; memset(&cc, 0, sizeof cc);
    cc.type_hash = 0x12345678u; strcpy(cc.type_name, "350Z"); cc.ride_b[3] = 1;
    for (int i = 0; i < RC_PART_SLOTS; i++){ cc.part[i].slot = (uint8_t)i; cc.part[i].key = rnd(); cc.part[i].owner = rnd(); }
    cc.n_parts = RC_PART_SLOTS;
    uint8_t rec[RC_CFG_MAX];
    int n = rc_carcfg_encode(&cc, rec, sizeof rec);
    CHECK(n == 27 + 9 * RC_PART_SLOTS, "full car record %d bytes", n);
    /* with the performance extension */
    { rc_carcfg cp = cc, d2; cp.has_perf = 1; for (int i = 0; i < RC_PERF_BYTES; i++) cp.perf[i] = (uint8_t)(i * 7 + 1);
      uint8_t r2[RC_CFG_MAX]; int n2 = rc_carcfg_encode(&cp, r2, sizeof r2);
      CHECK(n2 == 27 + 9 * RC_PART_SLOTS + 2 + RC_PERF_BYTES && r2[n - 1] == 1 && r2[n] == RC_EXT_PERF && r2[n + 1] == RC_PERF_BYTES,
            "car record with performance extension: %d bytes", n2);
      CHECK(rc_carcfg_decode(r2, n2, &d2) == RC_OK && d2.has_perf && !memcmp(d2.perf, cp.perf, RC_PERF_BYTES), "performance extension round trip");
      r2[n] = 77;                                            /* an unknown extension tag is skipped */
      CHECK(rc_carcfg_decode(r2, n2, &d2) == RC_OK && !d2.has_perf && d2.n_parts == RC_PART_SLOTS, "unknown extension skipped");
      /* and the horn after it */
      cp.has_horn = 1; cp.horn = 6; n2 = rc_carcfg_encode(&cp, r2, sizeof r2);
      CHECK(n2 == 27 + 9 * RC_PART_SLOTS + 2 + RC_PERF_BYTES + 3 && r2[n - 1] == 2 && r2[n2 - 3] == RC_EXT_HORN && r2[n2 - 2] == 1 && r2[n2 - 1] == 6,
            "car record with performance and horn extensions: %d bytes", n2);
      CHECK(rc_carcfg_decode(r2, n2, &d2) == RC_OK && d2.has_perf && d2.has_horn && d2.horn == 6, "horn extension round trip");
      r2[n2 - 3] = 77;                                       /* as an older core reads it: skipped */
      CHECK(rc_carcfg_decode(r2, n2, &d2) == RC_OK && d2.has_perf && !d2.has_horn, "horn extension skipped as unknown");
      cp.has_perf = 0; n2 = rc_carcfg_encode(&cp, r2, sizeof r2);
      CHECK(n2 == n + 3 && rc_carcfg_decode(r2, n2, &d2) == RC_OK && !d2.has_perf && d2.has_horn && d2.horn == 6, "horn extension alone"); }
    CHECK(rc_carcfg_decode(rec, n, &c2) == RC_OK && c2.type_hash == cc.type_hash && !strcmp(c2.type_name, "350Z") && c2.n_parts == RC_PART_SLOTS &&
          !memcmp(c2.part, cc.part, sizeof cc.part), "car record round trip");
    /* record: version 1, type hash 4, name 16, ride bytes 4, count 1 = 26, then 9 per part */
    rec[26 + 9] = 0;                                          /* second slot = slot 0 again */
    CHECK(rc_carcfg_decode(rec, n, &c2) == RC_E_FIELD, "duplicate part slot refused");
    rec[26 + 9] = 200;
    CHECK(rc_carcfg_decode(rec, n, &c2) == RC_E_FIELD, "part slot >= 170 refused");
    rec[26 + 9] = 1;
    CHECK(rc_carcfg_decode(rec, n - 1, &c2) != RC_OK, "truncated car record refused");
    rec[5] = 0x07;
    CHECK(rc_carcfg_decode(rec, n, &c2) == RC_E_FIELD, "non-printable car type name refused");
}

/* ---- 3. hostile datagrams ----------------------------------------------------------------- */
static void t_fuzz(long iters){
    uint8_t b[1600], base[16][RC_MAX_DGRAM]; int bn[16];
    rc_msg m;
    /* one valid datagram of each type, to mutate */
    for (int t = 1; t <= 16; t++){
        memset(&m, 0, sizeof m); m.h.type = (uint8_t)t; m.h.sender = 9;
        if (t == RC_MSG_HELLO){ strcpy(m.u.hello.name, "abc"); m.u.hello.n_seen = 2; }
        if (t == RC_MSG_PEERS){ m.u.peers.n = 2; m.u.peers.e[0].ip = 0x7f000001u; m.u.peers.e[0].port = 1; m.u.peers.e[0].id = 1; m.u.peers.e[1] = m.u.peers.e[0]; }
        if (t == RC_MSG_SNAP){ m.u.snap.n = 3; for (int k = 0; k < 3; k++) m.u.snap.s[k].q[3] = 1; }
        static uint8_t d[1000]; if (t == RC_MSG_CFG){ m.u.cfg.nfrag = 1; m.u.cfg.total = 100; m.u.cfg.dlen = 100; m.u.cfg.data = d; }
        if (t == RC_MSG_DUEL){ m.u.duel.op = RC_DUEL_OFFER; m.u.duel.to = 1; m.u.duel.duel = 5; }
        if (t == RC_MSG_MAP){ m.u.map.nparts = 1; }
        if (t == RC_MSG_LOBBY){ m.u.lobby.op = RC_LOBBY_JOIN; m.u.lobby.to = 1; m.u.lobby.lobby = 5; m.u.lobby.race = 6; m.u.lobby.max = 4;
                                m.u.lobby.n = 2; m.u.lobby.ids[0] = 9; m.u.lobby.ids[1] = 10; }
        static uint8_t le[RC_LOBBIES_ENTRY] = { 1, 0, 2, 0, 1, 2, 3, 4, 2, 6, 0 };
        if (t == RC_MSG_LOBBIES){ m.u.lobbies.n = 1; m.u.lobbies.data = le; }
        if (t == RC_MSG_RACEREP){ m.u.racerep.lobby = 5; m.u.racerep.race = 6; m.u.racerep.players = 2; m.u.racerep.place = 1;
                                  m.u.racerep.factor = 1.f; strcpy(m.u.racerep.name, "350Z"); }
        if (t == RC_MSG_SAY){ m.u.say.phrase = 3; m.u.say.seq = 1; }
        if (t == RC_MSG_RACERES){ m.u.raceres.lobby = 5; m.u.raceres.race = 6; m.u.raceres.n = 2; m.u.raceres.row[0].id = 9;
                                  m.u.raceres.row[0].flags = RC_RS_FINISHED | RC_RS_OURS; m.u.raceres.row[0].time = 60.f;
                                  m.u.raceres.row[0].done = 2.f; m.u.raceres.row[1].id = 10; m.u.raceres.row[1].done = 1.5f; }
        bn[t - 1] = rc_encode(&m, base[t - 1], RC_MAX_DGRAM);
    }
    rc_session s; rc_config cf; memset(&cf, 0, sizeof cf); cf.self_id = 1; cf.port = 1; cf.max_players = 8; cf.rate_hz = 20; cf.test_mode = 1;
    cf.duels = 1; cf.lobbies = 1; cf.say = 1;
    rc_io io = { 0, 0, 0, 0 };
    rc_session_init(&s, &cf, &io);
    long accepted = 0, refused = 0, bad = 0;
    for (long it = 0; it < iters; it++){
        int len;
        int kind = (int)(rnd() % 4);
        if (kind == 0){ len = (int)(rnd() % 1300); for (int i = 0; i < len; i++) b[i] = (uint8_t)rnd(); }
        else {
            int t = (int)(rnd() % 16); len = bn[t]; memcpy(b, base[t], (size_t)len);
            int nm = 1 + (int)(rnd() % 6);
            for (int k = 0; k < nm; k++) b[rnd() % (uint32_t)len] = (uint8_t)rnd();
            if (kind == 2) len = (int)(rnd() % (uint32_t)(len + 1));
            if (kind == 3){                                    /* keep the CRC valid: field fuzzing past the checksum */
                if (len >= 20){ uint32_t c = rc_crc32(b, (uint32_t)len - 4); b[6] = (uint8_t)len; b[7] = (uint8_t)(len >> 8);
                    c = rc_crc32(b, (uint32_t)len - 4); b[len - 4] = (uint8_t)c; b[len - 3] = (uint8_t)(c >> 8); b[len - 2] = (uint8_t)(c >> 16); b[len - 1] = (uint8_t)(c >> 24);
                    b[0] = 'N'; b[1] = '2'; b[2] = 'F'; b[3] = 'R'; b[4] = 1; }
            }
        }
        int e = rc_decode(b, len, &m);
        if (e == RC_OK){
            accepted++;
            int okk = m.h.len == len && len <= RC_MAX_DGRAM;
            if (m.h.type == RC_MSG_HELLO) okk = okk && strlen(m.u.hello.name) <= RC_NAME_MAX && m.u.hello.n_seen <= RC_MAX_PEERS;
            if (m.h.type == RC_MSG_PEERS) okk = okk && m.u.peers.n <= RC_MAX_PEERS;
            if (m.h.type == RC_MSG_SNAP){ okk = okk && m.u.snap.n >= 1 && m.u.snap.n <= 3;
                for (int k = 0; k < m.u.snap.n && okk; k++) for (int i = 0; i < 3; i++)
                    okk = okk && isfinite(m.u.snap.s[k].p[i]) && fabsf(m.u.snap.s[k].p[i]) <= 20000.f; }
            if (m.h.type == RC_MSG_CFG) okk = okk && m.u.cfg.data >= b && m.u.cfg.data + m.u.cfg.dlen <= b + len - 4;
            if (m.h.type == RC_MSG_DUEL) okk = okk && m.u.duel.op >= 1 && m.u.duel.op < RC_DUEL__N && m.u.duel.to && m.u.duel.duel;
            if (m.h.type == RC_MSG_LOBBY){ const rc_lobby *l = &m.u.lobby;
                okk = okk && l->op >= 1 && l->op < RC_LOBBY__N && l->lobby && l->race && l->max >= 2 && l->max <= RC_LOBBY_MAX && l->n <= l->max;
                for (int k = 0; k < l->n && okk; k++) okk = l->ids[k] != 0; }
            if (m.h.type == RC_MSG_LOBBIES) okk = okk && m.u.lobbies.n <= RC_LOBBIES_MAX && m.u.lobbies.data + (size_t)m.u.lobbies.n * RC_LOBBIES_ENTRY <= b + len - 4;
            if (!okk){ bad++; if (bad < 5) printf("  fuzz: accepted datagram breaks an invariant (type %d)\n", m.h.type); }
        } else refused++;
        /* and through a session (state machine, reassembly) */
        rc_receive(&s, 0x7f000001u + (rnd() % 4u), (uint16_t)(1 + rnd() % 3), b, len, it * 1000);
        if (it % 1000 == 0) rc_tick(&s, it * 1000);
    }
    CHECK(bad == 0, "fuzz: %ld accepted datagrams break invariants", bad);
    CHECK(s.n_duel_in == 0, "fuzz: no DUEL from a stranger reaches the game (%d)", s.n_duel_in);
    CHECK(s.n_lobby_in == 0 && s.lobbies_gen == 0, "fuzz: no LOBBY from a stranger, no LOBBIES from a non-hub (%d, %u)", s.n_lobby_in, s.lobbies_gen);
    CHECK(s.n_say_in == 0, "fuzz: no SAY from a stranger reaches the game (%d)", s.n_say_in);
    printf("  fuzz: %ld datagrams, %ld accepted, %ld refused, session rx %u bad %u policy %u\n", iters, accepted, refused, s.rx, s.rx_bad, s.rx_policy);
}

/* ---- 4. a fake network of sessions -------------------------------------------------------------- */
#define NN 6
static rc_session g_n[NN];
static int64_t g_now;
static int g_drop[NN][NN];                 /* 1 = link a -> b down */
static int g_loss_pct;
typedef struct { int64_t at; int to; uint32_t ip; uint16_t port; int len; uint8_t b[RC_MAX_DGRAM]; } Pkt;
static Pkt g_q[20000]; static int g_qn;
static int g_lat_us = 1000, g_jit_us = 0, g_dup_pct = 0;
static int g_ver_from = -1;                 /* this node's datagrams go out as protocol version 2 */
static int g_twoaddr_from = -1;             /* this node's HELLOs also arrive from 127.0.0.2 */
static int g_nox_from = -1;                 /* this node's HELLOs go out without RC_HF_STATEX (an older core) */
static int g_nocrew_from = -1;              /* this node's HELLOs go out without RC_HF_CREW (an older core) */
static int g_crew_to_old;                   /* HELLOs with crews sent to that node */
static int g_crew_p2p;                      /* HELLOs with crews sent from one node to another (with a hub: none) */
static int g_snapx_rx[NN], g_snap_rx[NN];   /* SNAPs each node was sent with and without state extensions */
static uint32_t node_ip(int i){ return 0x7f000001u; (void)i; }
static uint16_t node_port(int i){ return (uint16_t)(50000 + i); }
static int node_of(uint32_t ip, uint16_t port){ if (ip != 0x7f000001u) return -1; for (int i = 0; i < NN; i++) if (node_port(i) == port) return i; return -1; }
static int g_bcast_calls;
static int g_cfg_tx[NN];                    /* CFG datagrams each node sent */
/* a stand-in for the server's free-roam hub (web/server-rs/crates/roam): it
 * counts what each node sends it and, while on, lists g_hub_link[i] for node
 * i every second */
#define HUB_IP   0x7f0000FDu
#define HUB_PORT 47210
static int g_hub_on, g_hub_link[NN][NN], g_hub_hello[NN], g_hub_snap[NN], g_hub_snapx[NN], g_hub_bye[NN], g_hub_lobby[NN];
static int g_hub_ncrews[NN]; static uint32_t g_hub_crews[NN][RC_CREW_MAX];   /* each node's crews, as its last HELLO to the hub had them */
static uint16_t g_hub_flags[NN];            /* ...and its flags: with RC_HF_HUBCREW its lists end in crew marks */
static rc_lobby g_hub_lobby_last[NN];
static int64_t g_hub_next;
static void hub_in(int from, const uint8_t *b, int len){
    rc_msg m; if (rc_decode(b, len, &m) != RC_OK) return;
    if (m.h.type == RC_MSG_HELLO){ g_hub_hello[from]++; g_hub_flags[from] = m.h.flags;
                                   g_hub_ncrews[from] = m.u.hello.n_crews; memcpy(g_hub_crews[from], m.u.hello.crews, sizeof g_hub_crews[from]); }
    else if (m.h.type == RC_MSG_SNAP){ g_hub_snap[from]++; if (m.h.flags & RC_HF_STATEX) g_hub_snapx[from]++; }
    else if (m.h.type == RC_MSG_BYE) g_hub_bye[from]++;
    else if (m.h.type == RC_MSG_LOBBY){ g_hub_lobby[from]++; g_hub_lobby_last[from] = m.u.lobby; }
}
static void net_send(void *u, uint32_t ip, uint16_t port, const uint8_t *buf, int len){
    if (ip == HUB_IP && port == HUB_PORT){ hub_in((int)(intptr_t)u, buf, len); return; }
    int from = (int)(intptr_t)u, to = node_of(ip, port);
    if (len > 5 && buf[5] == RC_MSG_CFG) g_cfg_tx[from]++;
    if (to >= 0 && len > 15 && buf[5] == RC_MSG_SNAP){ if (buf[14] & RC_HF_STATEX) g_snapx_rx[to]++; else g_snap_rx[to]++; }
    if (to < 0 || g_drop[from][to]) return;
    if (to == g_nocrew_from && len > 15 && buf[5] == RC_MSG_HELLO){ rc_msg d; if (rc_decode(buf, len, &d) == RC_OK && d.u.hello.n_crews) g_crew_to_old++; }
    if (len > 15 && buf[5] == RC_MSG_HELLO){ rc_msg d; if (rc_decode(buf, len, &d) == RC_OK && d.u.hello.n_crews) g_crew_p2p++; }
    if (g_loss_pct && (int)(rnd() % 100u) < g_loss_pct) return;
    for (int copy = 0; copy < 2; copy++){
        if (copy && (int)(rnd() % 100u) >= g_dup_pct) break;
        if (g_qn >= 20000) return;
        Pkt *p = &g_q[g_qn++];
        p->at = g_now + g_lat_us + (g_jit_us ? (int)(rnd() % (uint32_t)(2 * g_jit_us)) - g_jit_us : 0);
        p->to = to; p->ip = node_ip(from); p->port = node_port(from); p->len = len; memcpy(p->b, buf, (size_t)len);
        if (from == g_ver_from){                /* another protocol version, CRC fixed up */
            p->b[4] = 2; uint32_t c = rc_crc32(p->b, (uint32_t)len - 4);
            for (int k = 0; k < 4; k++) p->b[len - 4 + k] = (uint8_t)(c >> (8 * k)); }
        if (from == g_nox_from && len > 15 && buf[5] == RC_MSG_HELLO){
            p->b[14] &= (uint8_t)~RC_HF_STATEX; uint32_t c = rc_crc32(p->b, (uint32_t)len - 4);
            for (int k = 0; k < 4; k++) p->b[len - 4 + k] = (uint8_t)(c >> (8 * k)); }
        if (from == g_nocrew_from && len > 15 && buf[5] == RC_MSG_HELLO){
            p->b[15] &= (uint8_t)~(RC_HF_CREW >> 8); uint32_t c = rc_crc32(p->b, (uint32_t)len - 4);
            for (int k = 0; k < 4; k++) p->b[len - 4 + k] = (uint8_t)(c >> (8 * k)); }
        if (from == g_twoaddr_from && len > 5 && buf[5] == RC_MSG_HELLO && g_qn < 20000){
            g_q[g_qn] = *p; g_q[g_qn].ip = 0x7f000002u; g_qn++; }
    }
}
static void net_bcast(void *u, uint16_t port, const uint8_t *buf, int len){ (void)u; (void)port; (void)buf; (void)len; g_bcast_calls++; }
static int g_join[NN], g_leave[NN], g_note[NN], g_restart[NN];
static char g_note_text[NN][200], g_leave_text[NN][200];
static void net_event(void *u, int ev, int peer, const char *t){
    (void)peer; int i = (int)(intptr_t)u;
    if (ev == RC_EV_JOIN) g_join[i]++;
    if (ev == RC_EV_LEAVE){ g_leave[i]++; snprintf(g_leave_text[i], sizeof g_leave_text[i], "%s", t); }
    if (ev == RC_EV_RESTART) g_restart[i]++;
    if (ev == RC_EV_NOTE){ g_note[i]++; snprintf(g_note_text[i], sizeof g_note_text[i], "%s", t); }
}
static void node_init(int i, uint32_t id, int maxp, uint32_t room){
    rc_config cf; memset(&cf, 0, sizeof cf);
    cf.self_id = id; cf.port = node_port(i); cf.room = room; snprintf(cf.name, sizeof cf.name, "node%d", i);
    cf.platform = RC_PLAT_TOOL; cf.test_mode = 1; cf.max_players = maxp; cf.rate_hz = 20;
    rc_io io = { (void *)(intptr_t)i, net_send, net_bcast, net_event };
    rc_session_init(&g_n[i], &cf, &io);
}
static void net_run(int64_t until, int nn){
    while (g_now < until){
        g_now += 1000;
        /* deliver everything due (in time order) */
        int moved = 1;
        while (moved){
            moved = 0; int best = -1;
            for (int k = 0; k < g_qn; k++) if (g_q[k].at <= g_now && (best < 0 || g_q[k].at < g_q[best].at)) best = k;
            if (best >= 0){
                Pkt p = g_q[best]; g_q[best] = g_q[--g_qn];
                if (p.to < nn) rc_receive(&g_n[p.to], p.ip, p.port, p.b, p.len, g_now);
                moved = 1;
            }
        }
        if (g_now % 10000 == 0) for (int i = 0; i < nn; i++) rc_tick(&g_n[i], g_now);
        if (g_hub_on && g_now >= g_hub_next){
            g_hub_next = g_now + 1000000;
            for (int i = 0; i < nn && g_qn < 20000; i++){
                rc_msg m; memset(&m, 0, sizeof m); m.h.type = RC_MSG_PEERS; m.h.sender = 0xB0B;
                /* crew marks as the server's hub makes them: to a node that reads them and is in a crew */
                if ((g_hub_flags[i] & RC_HF_HUBCREW) && g_hub_ncrews[i] > 0){
                    m.h.flags = RC_HF_CREW; m.u.peers.n_mine = (uint8_t)g_hub_ncrews[i];
                    memcpy(m.u.peers.mine, g_hub_crews[i], sizeof m.u.peers.mine);
                }
                for (int j = 0; j < nn; j++)
                    if (g_hub_link[i][j] && m.u.peers.n < RC_MAX_PEERS){
                        m.u.peers.e[m.u.peers.n].ip = node_ip(j); m.u.peers.e[m.u.peers.n].port = node_port(j);
                        m.u.peers.e[m.u.peers.n].id = g_n[j].conf.self_id;
                        for (int c = 0; m.h.flags && c < g_hub_ncrews[i]; c++)
                            for (int d = 0; d < g_hub_ncrews[j]; d++)
                                if (g_hub_crews[j][d] == g_hub_crews[i][c]) m.u.peers.crew[m.u.peers.n] |= (uint8_t)(1u << c);
                        m.u.peers.n++; }
                Pkt *p = &g_q[g_qn++];
                p->at = g_now + g_lat_us; p->to = i; p->ip = HUB_IP; p->port = HUB_PORT; p->len = rc_encode(&m, p->b, sizeof p->b);
            }
        }
    }
}
static int est(int i){ return rc_established(&g_n[i]); }
static void t_session(void){
    memset(g_drop, 0, sizeof g_drop); g_qn = 0; g_now = 0;
    for (int i = 0; i < 3; i++) node_init(i, 0x100 + (uint32_t)i, 8, 0);
    /* a line: 0 knows 1, 1 knows 2; gossip must close the triangle */
    rc_add_peer(&g_n[0], node_ip(1), node_port(1), 1, 0);
    rc_add_peer(&g_n[1], node_ip(2), node_port(2), 1, 0);
    net_run(5000000, 3);
    CHECK(est(0) == 2 && est(1) == 2 && est(2) == 2, "3 nodes, chain of configured peers: full mesh by gossip (%d %d %d)", est(0), est(1), est(2));
    CHECK(g_bcast_calls == 0, "no broadcast with discovery off");
    /* car records */
    rc_carcfg cc; memset(&cc, 0, sizeof cc); cc.type_hash = 0xABCDEFu; strcpy(cc.type_name, "GTO"); cc.n_parts = 150;
    for (int i = 0; i < 150; i++){ cc.part[i].slot = (uint8_t)i; cc.part[i].key = 0x1000u + (uint32_t)i; cc.part[i].owner = 7; }
    uint8_t rec[RC_CFG_MAX]; int rn = rc_carcfg_encode(&cc, rec, sizeof rec);
    g_loss_pct = 30;                                          /* fragments get lost: CFG_REQ must fill the holes */
    rc_set_cfg(&g_n[0], rec, rn, g_now);
    net_run(g_now + 6000000, 3);
    g_loss_pct = 0;
    net_run(g_now + 2000000, 3);
    int i01 = -1; for (int k = 0; k < RC_TABLE; k++) if (g_n[1].peer[k].id == 0x100) i01 = k;
    CHECK(i01 >= 0 && g_n[1].peer[i01].cfg_len == rn && !memcmp(g_n[1].peer[i01].cfg, rec, (size_t)rn) && g_n[1].peer[i01].cfg_rev == g_n[0].cfg_rev,
          "car record (%d bytes, 2 fragments) arrives through 30%% loss", rn);
    /* snapshots flow */
    for (int k = 0; k < 40; k++){
        rc_state st; memset(&st, 0, sizeof st); st.q[3] = 1; st.t_us = (uint32_t)g_now; st.p[0] = (float)k;
        rc_set_status(&g_n[0], RC_ST_ROAM); rc_push_state(&g_n[0], &st, g_now);
        net_run(g_now + 50000, 3);
    }
    CHECK(i01 >= 0 && g_n[1].peer[i01].rx_snap >= 35 && g_n[1].peer[i01].mo.n >= 20, "snapshots at 20 Hz (%u)", i01 >= 0 ? g_n[1].peer[i01].rx_snap : 0);
    /* a dead link: lost after 3 s, dropped after 10 s */
    for (int k = 0; k < 3; k++){ g_drop[0][k] = 1; g_drop[k][0] = 1; }
    int lv = g_leave[1];
    net_run(g_now + 4000000, 3);
    CHECK(i01 >= 0 && g_n[1].peer[i01].lost && g_n[1].peer[i01].state == RC_PS_EST, "silent peer: lost after 3 s");
    net_run(g_now + 7500000, 3);
    CHECK(g_leave[1] == lv + 1 && est(1) == 1, "silent peer: dropped after 10 s (leaves %d, est %d)", g_leave[1] - lv, est(1));
    /* the link comes back: the configured peer is tried again */
    for (int k = 0; k < 3; k++){ g_drop[0][k] = 0; g_drop[k][0] = 0; }
    net_run(g_now + 4000000, 3);
    CHECK(est(0) == 2 && est(1) == 2, "link back: re-established (%d %d)", est(0), est(1));
    /* restart: same address, new id */
    int j1 = g_join[1];
    node_init(2, 0x999, 8, 0);
    rc_add_peer(&g_n[2], node_ip(1), node_port(1), 1, g_now);
    net_run(g_now + 4000000, 3);
    int found = 0; for (int k = 0; k < RC_TABLE; k++) if (g_n[1].peer[k].id == 0x999 && g_n[1].peer[k].state == RC_PS_EST) found = 1;
    CHECK(found && g_join[1] > j1, "restarted peer (new id, same address) re-joins");
    /* BYE */
    int l0 = g_leave[0];
    rc_shutdown(&g_n[2], 0);
    net_run(g_now + 200000, 3);
    CHECK(g_leave[0] == l0 + 1, "BYE: dropped at once");
    /* rooms */
    memset(g_drop, 0, sizeof g_drop); g_qn = 0;
    node_init(0, 0x201, 8, rc_fnv1a("blue")); node_init(1, 0x202, 8, rc_fnv1a("red"));
    rc_add_peer(&g_n[0], node_ip(1), node_port(1), 1, g_now); rc_add_peer(&g_n[1], node_ip(0), node_port(0), 1, g_now);
    net_run(g_now + 3000000, 2);
    CHECK(est(0) == 0 && est(1) == 0, "different rooms never establish");
    /* max players */
    g_qn = 0;
    for (int i = 0; i < 4; i++) node_init(i, 0x300 + (uint32_t)i, 3, 0);
    for (int i = 1; i < 4; i++){ rc_add_peer(&g_n[0], node_ip(i), node_port(i), 1, g_now); rc_add_peer(&g_n[i], node_ip(0), node_port(0), 1, g_now); }
    net_run(g_now + 4000000, 4);
    CHECK(est(0) == 2, "max_players 3: two peers, not three (%d)", est(0));
    /* address policy: production accepts private ranges only */
    rc_session p; rc_config cf; memset(&cf, 0, sizeof cf); cf.self_id = 5; cf.port = 1; cf.max_players = 8; cf.rate_hz = 20;
    rc_io io = { 0, 0, 0, 0 }; rc_session_init(&p, &cf, &io);
    rc_msg m; memset(&m, 0, sizeof m); m.h.type = RC_MSG_HELLO; m.h.sender = 77; strcpy(m.u.hello.name, "x");
    uint8_t b[RC_MAX_DGRAM]; int n = rc_encode(&m, b, sizeof b);
    rc_receive(&p, 0x08080808u, 47210, b, n, 0);
    CHECK(p.rx_policy == 1 && p.peer[0].state == RC_PS_FREE, "public source address refused");
    rc_receive(&p, 0xC0A8010Au, 47210, b, n, 0);
    CHECK(p.peer[0].state == RC_PS_HEARD, "RFC 1918 source heard");
    p.conf.test_mode = 1;
    rc_receive(&p, 0x0A000001u, 47210, b, n, 0);
    CHECK(p.rx_policy == 2, "test mode: 10/8 refused");
    p.conf.test_mode = 0;
    rc_add_peer(&p, 0x64400005u, 47210, 1, 0);                  /* a listed VPN overlay address (100.64/10) */
    m.h.sender = 78; n = rc_encode(&m, b, sizeof b);
    rc_receive(&p, 0x64400005u, 47210, b, n, 0);
    CHECK(p.rx_policy == 2, "a listed non-private peer is heard");
    rc_receive(&p, 0x64400006u, 47210, b, n, 0);
    CHECK(p.rx_policy == 3, "an unlisted non-private address is not");
    /* rate limit: 300 datagrams in one second from one source */
    rc_session_init(&p, &cf, &io);
    for (int k = 0; k < 300; k++){ m.h.seq = (uint16_t)k; n = rc_encode(&m, b, sizeof b); rc_receive(&p, 0xC0A8010Au, 47210, b, n, 1000 + k); }
    CHECK(p.rx_ratelimit == 300 - 100, "rate limit: 100 per second per source (%u dropped)", p.rx_ratelimit);
    /* our own beacon */
    rc_session_init(&p, &cf, &io);
    m.h.sender = 5; n = rc_encode(&m, b, sizeof b); rc_receive(&p, 0xC0A8010Au, 47210, b, n, 0);
    CHECK(p.peer[0].state == RC_PS_FREE, "own beacon ignored");
    /* discovery: the beacon goes to io.broadcast every 2 s */
    cf.discovery = 1; g_bcast_calls = 0; rc_io io2 = { 0, net_send, net_bcast, 0 };
    rc_session_init(&p, &cf, &io2);
    for (int64_t t = 0; t <= 10000000; t += 10000) rc_tick(&p, t);
    CHECK(g_bcast_calls == 6, "discovery beacon every 2 s (%d in 10 s)", g_bcast_calls);
}

/* ---- 4b. the free-roam adversarial review's findings -- */
static int peer_of(int i, uint32_t id){ for (int k = 0; k < RC_TABLE; k++) if (g_n[i].peer[k].state != RC_PS_FREE && g_n[i].peer[k].id == id) return k; return -1; }
static void mesh(int n, int maxp){
    memset(g_drop, 0, sizeof g_drop); g_qn = 0; g_loss_pct = 0; g_ver_from = -1; g_twoaddr_from = -1;
    memset(g_join, 0, sizeof g_join); memset(g_leave, 0, sizeof g_leave); memset(g_note, 0, sizeof g_note); memset(g_restart, 0, sizeof g_restart);
    for (int i = 0; i < n; i++) node_init(i, 0x500 + (uint32_t)i, maxp, 0);
}
static void drive(int n, int64_t until){
    /* every node in free roam, a snapshot due every 50 ms */
    while (g_now < until){
        for (int i = 0; i < n; i++){
            rc_state st; memset(&st, 0, sizeof st); st.q[3] = 1; st.t_us = (uint32_t)g_now; st.p[0] = (float)i;
            rc_set_status(&g_n[i], RC_ST_ROAM); rc_push_state(&g_n[i], &st, g_now);
        }
        net_run(g_now + 10000, n);
    }
}
/* ---- 4c. the server's free-roam hub: its list is who the peers are -- */
static void hub_links(int a, int b){ g_hub_link[a][b] = g_hub_link[b][a] = 1; }
static void t_hub(void){
    mesh(4, 8);
    g_hub_on = 1; g_hub_next = 0; memset(g_hub_link, 0, sizeof g_hub_link);
    memset(g_hub_hello, 0, sizeof g_hub_hello); memset(g_hub_snap, 0, sizeof g_hub_snap); memset(g_hub_bye, 0, sizeof g_hub_bye);
    for (int i = 0; i < 4; i++){ g_n[i].conf.hub_ip = HUB_IP; g_n[i].conf.hub_port = HUB_PORT; g_n[i].conf.discovery = 1; }
    uint32_t id[4]; for (int i = 0; i < 4; i++) id[i] = g_n[i].conf.self_id;
    hub_links(0, 1); hub_links(2, 3);
    g_bcast_calls = 0;
    int64_t t0 = g_now;
    drive(4, g_now + 4000000);
    CHECK(est(0) == 1 && est(1) == 1 && est(2) == 1 && est(3) == 1 && peer_of(0, id[1]) >= 0 && peer_of(0, id[2]) < 0,
          "hub: the pairs it lists establish, nobody else (%d %d %d %d)", est(0), est(1), est(2), est(3));
    CHECK(g_bcast_calls == 0, "hub: no discovery broadcast while it answers (%d)", g_bcast_calls);
    CHECK(g_n[0].hub_was == 2, "hub: answering");
    CHECK(g_hub_hello[0] >= 3 && g_hub_snap[0] >= 6 && g_hub_snap[0] <= 9, "hub: a HELLO every second (%d) and a SNAP twice a second (%d) in %.0f s",
          g_hub_hello[0], g_hub_snap[0], (double)(g_now - t0) / 1e6);
    /* a player the hub does not list for us: its HELLOs are ignored */
    int k2 = rc_add_peer(&g_n[2], node_ip(0), node_port(0), 1, g_now);
    drive(4, g_now + 2000000);
    CHECK(g_n[0].rx_unlisted > 0 && peer_of(0, id[2]) < 0, "hub: an unlisted player's HELLO is ignored (%u)", g_n[0].rx_unlisted);
    if (k2 >= 0) g_n[2].peer[k2].state = RC_PS_FREE;
    /* the hub links others: the old peer goes (out of range), the new one comes */
    memset(g_hub_link, 0, sizeof g_hub_link); hub_links(0, 2); hub_links(1, 3);
    int lv = g_leave[0];
    drive(4, g_now + 1500000);
    int k1 = peer_of(0, id[1]);
    CHECK(k1 >= 0 && g_n[0].peer[k1].state == RC_PS_EST && !rc_peer_listed(&g_n[0], k1),
          "hub: a peer left out of its list stays a while, not listed (the game gives its car's slot away)");
    drive(4, g_now + 3500000);
    CHECK(peer_of(0, id[2]) >= 0 && g_n[0].peer[peer_of(0, id[2])].state == RC_PS_EST && peer_of(0, id[1]) < 0 && est(0) == 1,
          "hub: a new list, new peers (%d)", est(0));
    CHECK(rc_peer_listed(&g_n[0], peer_of(0, id[2])), "hub: the new peer is listed");
    CHECK(g_leave[0] == lv + 1 && strstr(g_leave_text[0], "out of range"), "hub: the unlisted peer left \"out of range\" ('%s')", g_leave_text[0]);
    /* the hub goes silent: discovery again after 10 s, nobody dropped for it */
    g_hub_on = 0; g_bcast_calls = 0;
    drive(4, g_now + 13000000);
    CHECK(g_bcast_calls > 0 && g_n[0].hub_was == 0 && est(0) == 1, "hub silent: discovery again, peers kept (%d broadcasts, state %d, %d peers)", g_bcast_calls, g_n[0].hub_was, est(0));
    CHECK(rc_peer_listed(&g_n[0], peer_of(0, id[2])), "hub silent: every peer counts as listed");
    /* it answers again; a BYE tells it we stop */
    g_hub_on = 1; g_hub_next = 0;
    drive(4, g_now + 2000000);
    CHECK(g_n[0].hub_was == 2, "hub back: answering again");
    rc_shutdown(&g_n[3], 0);
    CHECK(g_hub_bye[3] == 1, "hub: a BYE when we stop");
    g_hub_on = 0;
}

static void t_review(void){
    /* a late state of the previous epoch (a datagram sent before a teleport,
     * reordered) or of the previous clock epoch is dropped */
    { rc_interp ip; rc_interp_reset(&ip); rc_pose P; rc_state s; memset(&s, 0, sizeof s); s.q[3] = 1; s.v[0] = 10;
      int64_t t = 10000000;
      for (int k = 0; k < 40; k++){ s.t_us = (uint32_t)(k * 50000); s.p[0] = 0.5f * (float)k; rc_interp_insert(&ip, &s, 0, 0, RC_ST_ROAM, t + k * 50000 + 20000, 1); }
      t += 40 * 50000;
      for (int f = 0; f < 30; f++){ t += 8333; rc_interp_eval(&ip, t, 8333, 100000, 250000, &P); }
      s.p[0] = 1000.f; s.t_us = 40 * 50000; rc_interp_insert(&ip, &s, 1, 0, RC_ST_ROAM, t, 1);
      t += 8333; rc_interp_eval(&ip, t, 8333, 100000, 250000, &P); float tp = P.p[0];
      s.p[0] = 19.5f; s.t_us = 39 * 50000; rc_interp_insert(&ip, &s, 0, 0, RC_ST_ROAM, t + 1000, 1);
      t += 8333; rc_interp_eval(&ip, t, 8333, 100000, 250000, &P);
      CHECK(tp > 999.f && P.p[0] > 999.f && ip.n_stale == 1, "a late state of the old epoch after a teleport is dropped (shown %.1f, stale %llu)",
            (double)P.p[0], (unsigned long long)ip.n_stale);
      s.p[0] = 1001.f; s.t_us = 41 * 50000; rc_interp_insert(&ip, &s, 1, 1, RC_ST_ROAM, t + 2000, 1);      /* re-anchored clock */
      uint64_t ep = ip.n_epoch;
      s.p[0] = 5.f; s.t_us = 38 * 50000; rc_interp_insert(&ip, &s, 1, 0, RC_ST_ROAM, t + 3000, 1);         /* the old clock, late */
      CHECK(ip.clock == 1 && ip.n_epoch == ep && ip.n_stale == 2, "a late state of the old clock epoch is dropped too");
      s.p[0] = 2000.f; s.t_us = 5; rc_interp_insert(&ip, &s, 0, 0, RC_ST_ROAM, t + 4000, 1);              /* 255 epochs on: wraps */
      CHECK(ip.epoch == 1 && ip.n_stale == 3, "epoch order is modulo 256 (an epoch 255 behind is old)");
      s.p[0] = 3000.f; s.t_us = 6; rc_interp_insert(&ip, &s, 2, 0, RC_ST_ROAM, t + 5000, 1);
      CHECK(ip.epoch == 2 && ip.n == 1, "a newer epoch is taken"); }
    /* another protocol version: noted (with the address), at most every 30 s */
    mesh(2, 8); g_now = 0;
    rc_add_peer(&g_n[0], node_ip(1), node_port(1), 1, 0); rc_add_peer(&g_n[1], node_ip(0), node_port(0), 1, 0);
    g_ver_from = 1;
    net_run(40000000, 2);
    CHECK(est(0) == 0 && g_n[0].rx_version > 30 && g_note[0] == 2 && strstr(g_note_text[0], "127.0.0.1:50001") && strstr(g_note_text[0], "version 2"),
          "another protocol version is noted, every 30 s (%d notes, %u datagrams: \"%s\")", g_note[0], g_n[0].rx_version, g_note_text[0]);
    /* another room: counted and noted */
    mesh(2, 8); g_now = 0;
    node_init(1, 0x601, 8, rc_fnv1a("red"));
    rc_add_peer(&g_n[1], node_ip(0), node_port(0), 1, 0);
    net_run(5000000, 2);
    CHECK(est(0) == 0 && g_n[0].rx_room >= 4 && g_note[0] == 1 && strstr(g_note_text[0], "another room"),
          "a HELLO from another room is counted (%u) and noted once (\"%s\")", g_n[0].rx_room, g_note_text[0]);
    /* snapshot loss statistics: 0 on a lossless 3-node mesh (other messages
     * to other peers make no gaps), and close to the truth with loss */
    mesh(3, 8); g_now = 0;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) if (i != j) rc_add_peer(&g_n[i], node_ip(j), node_port(j), 1, 0);
    drive(3, 30000000);
    uint32_t lost = 0, snaps = 0;
    for (int i = 0; i < 3; i++) for (int k = 0; k < RC_TABLE; k++) if (g_n[i].peer[k].state == RC_PS_EST){ lost += g_n[i].peer[k].seq_lost; snaps += g_n[i].peer[k].rx_snap; }
    CHECK(est(0) == 2 && snaps > 3000 && lost == 0, "snapshots lost on a lossless 3-node mesh: %u of %u", lost, snaps);
    uint32_t rx0 = 0; for (int k = 0; k < RC_TABLE; k++) if (g_n[1].peer[k].id == 0x500) rx0 = g_n[1].peer[k].rx_snap;
    int k10 = peer_of(1, 0x500); uint32_t lost0 = k10 >= 0 ? g_n[1].peer[k10].seq_lost : 0;
    g_loss_pct = 10;
    drive(3, g_now + 20000000);
    g_loss_pct = 0;
    uint32_t got = k10 >= 0 ? g_n[1].peer[k10].rx_snap - rx0 : 0, gone = k10 >= 0 ? g_n[1].peer[k10].seq_lost - lost0 : 0;
    double rate = got + gone ? 100.0 * gone / (got + gone) : 0;
    /* about 400 snapshots: one standard deviation is 1.5 points */
    CHECK(rate > 6.0 && rate < 14.0, "10%% loss on the wire: %.1f%% of snapshots counted lost (%u of %u)", rate, gone, got + gone);
    /* different max_players: the full one does not list a waiting peer, so
     * nobody joins one-sidedly */
    mesh(3, 8); g_now = 0;
    node_init(0, 0x500, 2, 0);
    rc_add_peer(&g_n[0], node_ip(1), node_port(1), 1, 0); rc_add_peer(&g_n[1], node_ip(0), node_port(0), 1, 0);
    drive(2, 3000000);
    rc_add_peer(&g_n[2], node_ip(0), node_port(0), 1, g_now);
    drive(3, g_now + 5000000);
    int c_a = peer_of(2, 0x500);
    CHECK(est(0) == 1 && c_a >= 0 && g_n[2].peer[c_a].state != RC_PS_EST && g_join[2] == 0,
          "MaxPlayers 2 (full) next to 8: the third does not join one-sidedly (its state for A %d, joins %d)", c_a >= 0 ? g_n[2].peer[c_a].state : -1, g_join[2]);
    /* one player heard from two addresses: one entry */
    mesh(2, 8); g_now = 0;
    rc_add_peer(&g_n[0], node_ip(1), node_port(1), 1, 0);
    g_twoaddr_from = 1;
    drive(2, 6000000);
    int nb = 0; for (int k = 0; k < RC_TABLE; k++) if (g_n[0].peer[k].state != RC_PS_FREE && g_n[0].peer[k].id == 0x501) nb++;
    CHECK(nb == 1 && g_join[0] == 1 && g_n[0].rx_dup_id > 0, "one player heard from two addresses: one entry (%d), one join (%d)", nb, g_join[0]);
    /* a stray datagram with another id from a friend's address is not a restart */
    g_twoaddr_from = -1;
    rc_msg m; memset(&m, 0, sizeof m); m.h.type = RC_MSG_PING; m.h.sender = 0xBAD; uint8_t b[RC_MAX_DGRAM];
    int n = rc_encode(&m, b, sizeof b);
    rc_receive(&g_n[0], node_ip(1), node_port(1), b, n, g_now);
    drive(2, g_now + 1000000);
    CHECK(g_leave[0] == 0 && g_restart[0] == 0 && est(0) == 1 && g_n[0].rx_stray == 1, "a stray datagram with another id from a friend's address: ignored, no leave");
    /* ... but a HELLO with a new id is a restart (and the player joins again) */
    node_init(1, 0x777, 8, 0); rc_add_peer(&g_n[1], node_ip(0), node_port(0), 1, g_now);
    drive(2, g_now + 4000000);
    CHECK(g_restart[0] == 1 && peer_of(0, 0x777) >= 0 && g_n[0].peer[peer_of(0, 0x777)].state == RC_PS_EST, "a HELLO with a new id from the same address: a restart");
    /* after BYE: the quitter's session sends nothing more, and its late
     * datagrams (one sent before the BYE, overtaken) do not make it join
     * again */
    mesh(2, 8); g_now = 0;
    rc_add_peer(&g_n[0], node_ip(1), node_port(1), 1, 0); rc_add_peer(&g_n[1], node_ip(0), node_port(0), 1, 0);
    drive(2, 3000000);
    { rc_msg h; memset(&h, 0, sizeof h); h.h.type = RC_MSG_HELLO; h.h.sender = 0x501; strcpy(h.u.hello.name, "node1");
      h.u.hello.n_seen = 1; h.u.hello.seen[0] = 0x500; h.u.hello.status = RC_ST_ROAM; uint8_t hb[RC_MAX_DGRAM]; int hn = rc_encode(&h, hb, sizeof hb);
      uint32_t tx1 = g_n[1].tx;
      rc_shutdown(&g_n[1], 0);
      uint32_t after = g_n[1].tx;
      net_run(g_now + 200000, 1);                            /* node 0 gets the BYE; node 1 is silent from now on */
      rc_tick(&g_n[1], g_now); rc_state st; memset(&st, 0, sizeof st); st.q[3] = 1; rc_push_state(&g_n[1], &st, g_now + 100000);
      rc_receive(&g_n[0], node_ip(1), node_port(1), hb, hn, g_now);   /* the overtaken HELLO */
      net_run(g_now + 1000000, 1);
      CHECK(after > tx1 && g_n[1].tx == after && est(0) == 0 && g_join[0] == 1 && g_leave[0] == 1 && g_n[0].rx_after_bye == 3,   /* the 2nd and 3rd BYE, the HELLO */
            "after BYE: silent (tx %u, %u -> %u), a late HELLO of the quitter ignored (joins %d, leaves %d, est %d, after bye %u)", tx1, after, g_n[1].tx, g_join[0], g_leave[0], est(0), g_n[0].rx_after_bye); }
    /* a car record this core cannot read: noted once, not asked for again */
    mesh(2, 8); g_now = 0;
    rc_add_peer(&g_n[0], node_ip(1), node_port(1), 1, 0);
    uint8_t rec[300]; memset(rec, 0, sizeof rec); rec[0] = 2;                 /* record version 2 */
    rc_set_cfg(&g_n[1], rec, sizeof rec, 0);
    drive(2, 2000000);
    int notes = g_note[0]; uint32_t tx1 = g_n[1].tx, tx0 = g_n[0].tx;
    drive(2, g_now + 10000000);
    /* in 10 s: 200 SNAPs, 5 HELLOs, 10 PINGs, 10 PONGs, 1 PEERS each way, and no CFG or CFG_REQ */
    int k01 = peer_of(0, 0x501);
    CHECK(notes == 1 && g_note[0] == 1 && k01 >= 0 && g_n[0].peer[k01].cfg_bad_rev == g_n[1].cfg_rev && g_n[0].tx - tx0 < 240 && g_n[1].tx - tx1 < 240,
          "an unreadable car record: one note (\"%s\"), not asked for again (tx %u / %u in 10 s)", g_note_text[0], g_n[0].tx - tx0, g_n[1].tx - tx1);
    rec[0] = 1; rec[5] = 'X';                                                 /* still not a valid record */
    uint8_t ok_rec[64]; rc_carcfg cc; memset(&cc, 0, sizeof cc); cc.type_hash = 5; strcpy(cc.type_name, "RX8");
    int okn = rc_carcfg_encode(&cc, ok_rec, sizeof ok_rec);
    rc_set_cfg(&g_n[1], ok_rec, okn, g_now);
    drive(2, g_now + 2000000);
    CHECK(k01 >= 0 && g_n[0].peer[k01].cfg_len == okn, "a new revision is fetched again");
    /* a peer asking for the car record without end (a 23-byte CFG_REQ, for
     * the current revision or another, each answered with the whole record):
     * answered at most every 250 ms (the security review: every one was) */
    { mesh(2, 8); g_now = 0;
      rc_add_peer(&g_n[0], node_ip(1), node_port(1), 1, 0); rc_add_peer(&g_n[1], node_ip(0), node_port(0), 1, 0);
      rc_carcfg big; memset(&big, 0, sizeof big); big.type_hash = 0x77u; strcpy(big.type_name, "SKYLINE"); big.n_parts = 150;
      for (int i = 0; i < 150; i++){ big.part[i].slot = (uint8_t)i; big.part[i].key = 0x2000u + (uint32_t)i; }
      uint8_t brec[RC_CFG_MAX]; int bn = rc_carcfg_encode(&big, brec, sizeof brec);
      rc_set_cfg(&g_n[0], brec, bn, 0);
      drive(2, 2000000);
      int k10 = peer_of(1, 0x500), c0 = g_cfg_tx[0], asked = 0;
      int64_t end = g_now + 2000000;
      while (g_now < end){
          rc_msg q; memset(&q, 0, sizeof q); q.h.type = RC_MSG_CFG_REQ; q.h.sender = 0x501;
          q.u.cfgreq.cfg_rev = (uint16_t)(g_n[0].cfg_rev + (uint16_t)(asked++ & 1));
          uint8_t qb[64]; int qn = rc_encode(&q, qb, sizeof qb);
          rc_receive(&g_n[0], node_ip(1), node_port(1), qb, qn, g_now);
          drive(2, g_now + 20000);
      }
      int frags = g_cfg_tx[0] - c0;
      CHECK(bn > RC_CFG_FRAG && k10 >= 0 && g_n[1].peer[k10].cfg_len == bn && est(0) == 1 && asked == 100 && frags <= 2 * 9,
            "a peer asking for the car record 50 times a second: %d fragments back for %d requests in 2 s", frags, asked); }
    /* encoder edge values: NaN and out-of-range floats give defined bytes
     * (the same on x86: tests/roam_selftest.c) */
    memset(&m, 0, sizeof m); m.h.type = RC_MSG_SNAP; m.h.sender = 1; m.u.snap.n = 1; m.u.snap.status = RC_ST_ROAM;
    rc_state *st = &m.u.snap.s[0]; st->q[3] = 1;
    float nanv = __builtin_nanf("");
    st->w[0] = nanv; st->w[1] = 3e6f; st->w[2] = -3e6f; st->speed = nanv; st->steer = nanv; st->rpm = nanv; st->throttle = nanv; st->brake = 1e30f;
    n = rc_encode(&m, b, sizeof b);
    const uint8_t *q = b + 22;
    CHECK(n > 0 && (int16_t)le16(q + 36) == 0 && (int16_t)le16(q + 38) == 32767 && (int16_t)le16(q + 40) == -32767 && (int16_t)le16(q + 42) == 0 &&
          q[44] == 0 && q[45] == 255 && le16(q + 46) == 0 && (int16_t)le16(q + 48) == 0,
          "NaN and huge values: angular velocity 0 / +32767 / -32767, steer 0, throttle 0, brake 255, rpm 0, speed 0");
}

/* ---- 5. interpolation against a known trajectory ------------------------------------------------- */
/* a car on a circle (radius 80 m, 45 m/s) with a slalom in z-rotation */
static void truth(double t, float *p, float *v, float *q, float *w){
    double R = 80.0, sp = 45.0, om = sp / R, a = om * t;
    p[0] = (float)(R * cos(a)); p[1] = (float)(R * sin(a)); p[2] = (float)(2.0 * sin(0.3 * t));
    v[0] = (float)(-sp * sin(a)); v[1] = (float)(sp * cos(a)); v[2] = (float)(0.6 * cos(0.3 * t));
    double yaw = a + M_PI / 2;
    q[0] = 0; q[1] = 0; q[2] = (float)sin(yaw / 2); q[3] = (float)cos(yaw / 2);
    w[0] = 0; w[1] = 0; w[2] = (float)om;
}
typedef struct { double mean, p99, max, extrap_frac; uint64_t snaps; double max_jerk, latency; } Metrics;
static int cmpd(const void *a, const void *b){ double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static Metrics run_interp(int loss_pct, int jitter_us, int dup_pct, int reorder, double drift_ppm, int64_t latency_us){
    rc_interp ip; rc_interp_reset(&ip);
    static double err[40000]; int ne = 0;
    /* sender samples at 20 Hz on its own clock; receiver renders at 120 Hz */
    const int64_t delay = 100000, cap = 250000;
    int64_t sender_off = 123456789;                          /* the clocks are unrelated */
    struct { int64_t at; rc_state s; } q[64]; int qn = 0;
    int64_t next_sample = 0; uint32_t sent = 0;
    rc_state hist[3]; int nh = 0;
    double prev_p[3] = {0}; int have_prev = 0; double max_jerk = 0, prev_step = 0, lat_sum = 0; long lat_n = 0;
    for (int64_t now = 0; now < 30000000; now += 8333){
        double ts = (double)now / 1e6;                       /* true time */
        if (now >= next_sample){
            next_sample += 50000;
            rc_state s; memset(&s, 0, sizeof s);
            int64_t st = sender_off + (int64_t)((double)now * (1.0 + drift_ppm * 1e-6));
            s.t_us = (uint32_t)st;
            truth(ts, s.p, s.v, s.q, s.w);
            memmove(&hist[1], &hist[0], sizeof hist[0] * 2); hist[0] = s; if (nh < 3) nh++;
            sent++;
            if ((int)(rnd() % 100u) >= loss_pct){
                for (int copy = 0; copy < 2; copy++){
                    if (copy && (int)(rnd() % 100u) >= dup_pct) break;
                    int64_t at = now + latency_us + (jitter_us ? (int64_t)(rnd() % (uint32_t)(2 * jitter_us)) - jitter_us : 0);
                    if (reorder && rnd() % 10u == 0) at += 60000;
                    if (qn < 64){ q[qn].at = at; q[qn].s = s; qn++; }
                    /* the SNAP also carries the 2 older states */
                    for (int k = 1; k < nh && qn < 64; k++){ q[qn].at = at; q[qn].s = hist[k]; q[qn].s.bits = 0x80; qn++; }
                }
            }
        }
        /* receiver: deliver due packets */
        for (int k = 0; k < qn; ){
            if (q[k].at <= now){ int newest = !(q[k].s.bits & 0x80); rc_state s = q[k].s; s.bits = 0;
                rc_interp_insert(&ip, &s, 0, 0, RC_ST_ROAM, now, newest); q[k] = q[--qn]; }
            else k++;
        }
        rc_pose P;
        if (!rc_interp_eval(&ip, now, 8333, delay, cap, &P)) continue;
        /* the truth at the time the receiver rendered (its render time back in
         * true time): this is the smoothing error; the latency is separate */
        int64_t rt = now - ip.off_r - delay;
        double tt = (double)(rt - ((int64_t)1 << 32) - sender_off) / (1.0 + drift_ppm * 1e-6) / 1e6;   /* unwrapped t starts at 2^32 */
        if (ts < 1.0) continue;
        lat_sum += ts - tt; lat_n++;
        float tp[3], tv[3], tq[4], tw[3]; truth(tt, tp, tv, tq, tw);
        double dx = P.p[0] - tp[0], dy = P.p[1] - tp[1], dz = P.p[2] - tp[2];
        if (ne < 40000) err[ne++] = sqrt(dx*dx + dy*dy + dz*dz);
        if (have_prev){
            double s0 = sqrt((P.p[0]-prev_p[0])*(P.p[0]-prev_p[0]) + (P.p[1]-prev_p[1])*(P.p[1]-prev_p[1]));
            if (prev_step > 0){ double j = fabs(s0 - prev_step); if (j > max_jerk) max_jerk = j; }
            prev_step = s0;
        }
        prev_p[0] = P.p[0]; prev_p[1] = P.p[1]; prev_p[2] = P.p[2]; have_prev = 1;
    }
    Metrics M; memset(&M, 0, sizeof M);
    double sum = 0; for (int i = 0; i < ne; i++){ sum += err[i]; if (err[i] > M.max) M.max = err[i]; }
    M.mean = ne ? sum / ne : 0;
    qsort(err, (size_t)ne, sizeof err[0], cmpd);
    M.p99 = ne ? err[(int)(ne * 0.99)] : 0;
    M.extrap_frac = ip.n_eval ? (double)ip.n_extrap / (double)ip.n_eval : 0;
    M.snaps = ip.n_snap; M.max_jerk = max_jerk; M.latency = lat_n ? lat_sum / lat_n : 0;
    return M;
}
static void t_interp(void){
    struct { const char *name; int loss, jit, dup, reord; double ppm; int64_t lat; double lim_p99, lim_max; } cs[] = {
        { "clean LAN (1 ms)",            0,     0, 0, 0,    0,   1000, 0.05, 0.10 },
        { "clock drift +200 ppm",        0,     0, 0, 0,  200,   1000, 0.10, 0.20 },
        { "clock drift -200 ppm",        0,     0, 0, 0, -200,   1000, 0.10, 0.20 },
        { "20 ms jitter",                0, 20000, 0, 0,    0,   5000, 0.30, 1.00 },
        { "5% loss, 20 ms jitter",       5, 20000, 0, 0,    0,   5000, 0.50, 1.50 },
        { "5% loss + 100 ms delay",      5, 10000, 0, 0,    0, 100000, 0.50, 1.50 },
        { "10% loss, duplicates, reorder",10, 10000, 10, 1, 0,   5000, 0.80, 3.00 },
    };
    for (unsigned i = 0; i < sizeof cs / sizeof cs[0]; i++){
        Metrics M = run_interp(cs[i].loss, cs[i].jit, cs[i].dup, cs[i].reord, cs[i].ppm, cs[i].lat);
        printf("  interp %-32s shown %.1f ms behind; error mean %.4f m, p99 %.4f m, max %.4f m; extrapolating %.2f%%; snaps %llu; "
               "max step change %.4f m\n", cs[i].name, 1000 * M.latency, M.mean, M.p99, M.max, 100 * M.extrap_frac, (unsigned long long)M.snaps, M.max_jerk);
        CHECK(M.p99 < cs[i].lim_p99 && M.max < cs[i].lim_max, "interp %s: p99 %.3f (< %.2f), max %.3f (< %.2f)", cs[i].name, M.p99, cs[i].lim_p99, M.max, cs[i].lim_max);
        if (cs[i].loss == 0 && cs[i].jit == 0) CHECK(M.extrap_frac < 0.01 && M.snaps == 0, "clean: < 1%% extrapolation, no snaps");
    }
    /* teleport epoch: the car snaps to the new place, no glide through the city */
    rc_interp ip; rc_interp_reset(&ip);
    rc_state s; memset(&s, 0, sizeof s); s.q[3] = 1;
    for (int k = 0; k < 10; k++){ s.t_us = (uint32_t)(k * 50000); s.p[0] = (float)k; s.v[0] = 20; rc_interp_insert(&ip, &s, 0, 0, RC_ST_ROAM, k * 50000, 1); }
    rc_pose P; for (int64_t t = 0; t < 600000; t += 8333) rc_interp_eval(&ip, t, 8333, 100000, 250000, &P);
    s.t_us = 600000; s.p[0] = 1000; rc_interp_insert(&ip, &s, 1, 0, RC_ST_ROAM, 600000, 1);
    rc_interp_eval(&ip, 608333, 8333, 100000, 250000, &P);
    CHECK(fabsf(P.p[0] - 1000.f) < 0.5f, "teleport epoch: snapped to %.1f", (double)P.p[0]);
    /* paused sender: hold, no extrapolation */
    rc_interp_reset(&ip);
    for (int k = 0; k < 4; k++){ s.t_us = (uint32_t)(k * 50000); s.p[0] = (float)k; s.v[0] = 20; rc_interp_insert(&ip, &s, 0, 0, k < 3 ? RC_ST_ROAM : RC_ST_PAUSED, k * 50000, 1); }
    rc_interp_eval(&ip, 1000000, 8333, 100000, 250000, &P);
    CHECK(P.p[0] == 3.f && P.mode == RC_MODE_HOLD, "paused sender: held at %.2f", (double)P.p[0]);
    /* a gap: extrapolate up to 250 ms, then hold */
    rc_interp_reset(&ip);
    for (int k = 0; k < 4; k++){ s.t_us = (uint32_t)(k * 50000); s.p[0] = (float)k; s.v[0] = 20; rc_interp_insert(&ip, &s, 0, 0, RC_ST_ROAM, k * 50000, 1); }
    rc_interp_eval(&ip, 150000 + 100000 + 100000, 8333, 100000, 250000, &P);
    CHECK(P.mode == RC_MODE_EXTRAP && fabsf(P.p[0] - (3.f + 20.f * 0.1f)) < 0.05f, "gap: extrapolated to %.2f", (double)P.p[0]);
    rc_interp_eval(&ip, 150000 + 100000 + 600000, 8333, 100000, 250000, &P);
    CHECK(P.mode == RC_MODE_HOLD, "gap > 250 ms: held");
    /* expneg */
    float worst = 0; for (float x = 0; x < 20; x += 0.01f){ float r = rc_expneg(x), t = expf(-x); float e = fabsf(r - t) / (t > 1e-30f ? t : 1e-30f); if (x < 15 && e > worst) worst = e; }
    CHECK(worst < 1e-4f, "rc_expneg relative error %.2e", (double)worst);
    /* atan2 (the heading of friends' cars): against libm, every quadrant and axis */
    {
        double aw = 0; int sameh = 0, nh = 0;
        for (int i = 0; i < 200000; i++){
            float ang = (float)i * (6.2831853f / 200000.f) - 3.1415926f, r = 0.5f + (float)(i % 97);
            float y = r * sinf(ang), x = r * cosf(ang);
            double e = fabs((double)rc_atan2f(y, x) - atan2((double)y, (double)x));
            if (e > 3.2 && e < 3.15e0 + 3.2) e = fabs(e - 6.283185307179586);
            if (e > aw) aw = e;
            uint16_t h0 = (uint16_t)(int)(atan2f(y, x) / (2.f * 3.14159265f) * 65536.f);
            uint16_t h1 = (uint16_t)(int)(rc_atan2f(y, x) / (2.f * 3.14159265f) * 65536.f);
            sameh += h0 == h1; nh++;
        }
        float ax[8][2] = { {0,1}, {0,-1}, {1,0}, {-1,0}, {0,0}, {1e-30f,1}, {-1,-1e-30f}, {3,3} };
        int axok = 1;
        for (int k = 0; k < 8; k++) if (fabs((double)rc_atan2f(ax[k][0], ax[k][1]) - atan2((double)ax[k][0], (double)ax[k][1])) > 3e-7) axok = 0;
        CHECK(aw < 4e-7 && axok, "rc_atan2f within %.2e rad of libm (axes %s)", aw, axok ? "exact" : "OFF");
        CHECK(sameh >= nh - nh / 1000, "heading u16 from rc_atan2f equals libm's in %d of %d cases", sameh, nh);
    }
    /* matrix <-> quaternion */
    float mx = 0;
    for (int it = 0; it < 10000; it++){
        float q[4] = { frnd(-1,1), frnd(-1,1), frnd(-1,1), frnd(-1,1) }, n = sqrtf(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
        for (int i = 0; i < 4; i++) q[i] /= n;
        float f[3], l[3], u[3], q2[4], f2[3], l2[3], u2[3];
        rc_q2m(q, f, l, u); rc_m2q(f, l, u, q2); rc_q2m(q2, f2, l2, u2);
        for (int i = 0; i < 3; i++){ mx = fmaxf(mx, fabsf(f[i]-f2[i])); mx = fmaxf(mx, fabsf(l[i]-l2[i])); mx = fmaxf(mx, fabsf(u[i]-u2[i])); }
    }
    CHECK(mx < 1e-5f, "matrix -> quaternion -> matrix: max %.2e", (double)mx);
}

/* ---- 6. the platform guard ----------------------------------------------------------------- */
static void t_guard(void){
    printf("  (the next BLOCKED lines are expected: the guard under test)\n");
    CHECK(roam_plat_guard(1, 0x7f000001u, 0) == 1, "test mode: 127.0.0.1 allowed");
    CHECK(roam_plat_guard(1, 0xC0A80114u, 0) == 0, "test mode: 192.168.1.20 refused");
    CHECK(roam_plat_guard(1, 0xFFFFFFFFu, 1) == 0, "test mode: broadcast refused");
    CHECK(roam_plat_guard(1, 0x7fffffffu, 1) == 0, "test mode: even a loopback 'broadcast' refused");
    CHECK(roam_plat_guard(0, 0xC0A80114u, 0) == 1, "production: LAN address allowed");
    roam_if ifs[] = {
        { "lo0",   0x7f000001u, 0xff000000u, 1, 1, 0 },
        { "en0",   0xC0A80114u, 0xffffff00u, 1, 0, 1 },
        { "en1",   0x0A000005u, 0xff000000u, 1, 0, 1 },
        { "utun3", 0x64400001u, 0xffc00000u, 1, 0, 1 },
        { "awdl0", 0xA9FE0101u, 0xffff0000u, 1, 0, 1 },
        { "en5",   0x55AA0001u, 0xffffff00u, 1, 0, 1 },      /* public address: no */
        { "en2",   0xC0A80220u, 0xffffff00u, 0, 0, 1 },      /* down */
        { "bridge0", 0xC0A80201u, 0xffffff00u, 1, 0, 1 },
    };
    uint32_t t[16]; int n = roam_bcast_targets(ifs, (int)(sizeof ifs / sizeof ifs[0]), t, 16);
    CHECK(n == 4 && t[0] == 0xFFFFFFFFu && t[1] == 0xC0A801FFu && t[2] == 0x0AFFFFFFu && t[3] == 0xC0A802FFu,
          "broadcast targets: limited + en0 + en1 + bridge0 (%d)", n);
    uint32_t ip[8]; uint16_t pt[8];
    n = roam_plat_parse_peers("127.0.0.1:47211, 192.168.1.20, 127.0.0.2,host.local:5,1.2.3:4, 127.0.0.1:0", 1, 47210, ip, pt, 8);
    CHECK(n == 2 && ip[0] == 0x7f000001u && pt[0] == 47211 && ip[1] == 0x7f000002u && pt[1] == 47210, "peer list, test mode: loopback literals only (%d)", n);
    n = roam_plat_parse_peers("192.168.1.20, 10.0.0.7:47300", 0, 47210, ip, pt, 8);
    CHECK(n == 2 && ip[0] == 0xC0A80114u && pt[0] == 47210 && pt[1] == 47300, "peer list, production");
    /* settings.ini's check (bundle.m) and the Windows plugin take tabs as separators too */
    n = roam_plat_parse_peers("127.0.0.1:48761\t127.0.0.1:48762,\t127.0.0.1:48763", 1, 47210, ip, pt, 8);
    CHECK(n == 3 && pt[0] == 48761 && pt[1] == 48762 && pt[2] == 48763, "peer list: tab-separated entries (%d)", n);
    char nm[RC_NAME_MAX + 1];
    rc_name_clean(nm, "  Th\xe9o\x01 ", 10); CHECK(!strcmp(nm, "Th?o?"), "name cleaned: '%s'", nm);
    rc_name_clean(nm, "", 0); CHECK(!strcmp(nm, "Player"), "empty name -> Player");
    rc_name_clean(nm, "abcdefghijklmnopqrstuvwxyz0123", 30); CHECK(strlen(nm) == RC_NAME_MAX, "name cut at 24");
    rc_name_clean(nm, "f4g_42", 6); CHECK(!strcmp(nm, "Player"), "a name that reads as a slur -> Player ('%s')", nm);
}

/* ---- 4d. states per SNAP (rc_config.snap_states): 3 on a LAN, 1 over the browser's WebSocket -- */
static int g_cap_len, g_cap_n;
static void cap_send(void *u, uint32_t ip, uint16_t port, const uint8_t *b, int n){
    (void)u; (void)ip; (void)port;
    if (n > 20 && b[5] == RC_MSG_SNAP){ g_cap_len = n; g_cap_n = b[20]; }
}
static void t_snapstates(void){
    for (int want = 0; want <= 1; want++){
        rc_session p; rc_config cf; memset(&cf, 0, sizeof cf);
        cf.self_id = 9; cf.port = 1; cf.max_players = 8; cf.rate_hz = 20; cf.snap_states = want;
        cf.hub_ip = 0x0A4D00FDu; cf.hub_port = 1;               /* the hub gets a SNAP twice a second */
        rc_io io = { 0, cap_send, 0, 0 }; rc_session_init(&p, &cf, &io);
        g_cap_len = g_cap_n = 0;
        for (int64_t t = 0; t <= 2000000; t += 50000){
            rc_state st; memset(&st, 0, sizeof st); st.q[3] = 1; st.t_us = (uint32_t)t;
            rc_set_status(&p, RC_ST_ROAM); rc_push_state(&p, &st, t);
        }
        int n = want ? 1 : RC_SNAP_STATES, len = RC_HDR + 6 + n * RC_STATE_BYTES + RC_CRC;
        CHECK(g_cap_n == n && g_cap_len == len, "snap_states %d: SNAP with %d state(s), %d bytes (%d, %d bytes)", want, n, len, g_cap_n, g_cap_len);
    }
    /* a peer that sends one state per SNAP: its car moves as smoothly, through 5 % loss */
    mesh(2, 8);
    g_n[0].conf.snap_states = 1;
    rc_add_peer(&g_n[0], node_ip(1), node_port(1), 1, g_now); rc_add_peer(&g_n[1], node_ip(0), node_port(0), 1, g_now);
    net_run(g_now + 2000000, 2);
    g_loss_pct = 5;
    drive(2, g_now + 4000000);
    g_loss_pct = 0;
    int k = peer_of(1, 0x500);
    CHECK(k >= 0 && g_n[1].peer[k].rx_snap >= 60 && g_n[1].peer[k].mo.n >= 20 && g_n[1].peer[k].mo.n_dup == 0,
          "one state per SNAP: %u snapshots, %d buffered, no duplicates", k >= 0 ? g_n[1].peer[k].rx_snap : 0, k >= 0 ? g_n[1].peer[k].mo.n : 0);
}

/* ---- 4e. groups past 8: lists of up to RC_MAX_PEERS, 7 for a core of version 1 -- */
static struct { int hello_n, hello_self, peers_n, hellos, peers, snaps, every; } g_big[40];
static void big_send(void *u, uint32_t ip, uint16_t port, const uint8_t *b, int n){
    (void)u; (void)ip;
    rc_msg m; int k = port - 51000;
    if (k < 0 || k >= 40 || rc_decode(b, n, &m) != RC_OK) return;
    if (m.h.type == RC_MSG_HELLO){
        g_big[k].hellos++; g_big[k].hello_n = m.u.hello.n_seen; g_big[k].hello_self = 0;
        for (int i = 0; i < m.u.hello.n_seen; i++) if (m.u.hello.seen[i] == 0x1000u + (uint32_t)k) g_big[k].hello_self = 1;
    }
    if (m.h.type == RC_MSG_PEERS){ g_big[k].peers++; g_big[k].peers_n = m.u.peers.n; }
    if (m.h.type == RC_MSG_SNAP){ g_big[k].snaps++; g_big[k].every = m.h.flags & RC_HF_EVERY; }
}
enum { BIG_NP = 20, BIG_OLD = 3 };
/* the BIG_NP players' HELLOs, each hearing us; player BIG_OLD speaks core version 1 */
static void big_hellos(rc_session *s, int64_t now){
    for (int i = 0; i < BIG_NP; i++){
        rc_msg m; memset(&m, 0, sizeof m); m.h.type = RC_MSG_HELLO; m.h.sender = 0x1000u + (uint32_t)i;
        m.u.hello.core_ver = i == BIG_OLD ? 1 : RC_CORE_VER; m.u.hello.status = RC_ST_ROAM; m.u.hello.listen_port = (uint16_t)(51000 + i);
        snprintf(m.u.hello.name, sizeof m.u.hello.name, "p%d", i); m.u.hello.n_seen = 1; m.u.hello.seen[0] = 0x77;
        uint8_t b[RC_MAX_DGRAM]; int n = rc_encode(&m, b, sizeof b);
        rc_receive(s, 0x7f000001u, (uint16_t)(51000 + i), b, n, now);
    }
}
static void t_bigmesh(void){
    enum { NP = BIG_NP, OLD = BIG_OLD };
    static rc_session s; rc_config cf; memset(&cf, 0, sizeof cf);
    cf.self_id = 0x77; cf.port = 50999; cf.platform = RC_PLAT_TOOL; cf.test_mode = 1; cf.max_players = RC_MAX_PLAYERS; cf.rate_hz = 20;
    rc_io io = { 0, big_send, 0, 0 }; rc_session_init(&s, &cf, &io);
    memset(g_big, 0, sizeof g_big);
    int64_t now = 1000000;
    big_hellos(&s, now); now += 200000; big_hellos(&s, now); now += 200000;
    CHECK(rc_established(&s) == NP, "a group of %d: every player established (%d)", NP + 1, rc_established(&s));
    memset(g_big, 0, sizeof g_big);
    for (int64_t t = now; t < now + 11000000; t += 100000){                    /* HELLOs every 2 s, PEERS every 10 s */
        if ((t - now) % 1000000 == 0) big_hellos(&s, t);
        rc_tick(&s, t);
    }
    int ok_new = 1, ok_old = g_big[OLD].hellos > 0 && g_big[OLD].hello_n <= RC_PEERS_V1 && g_big[OLD].hello_self &&
                             g_big[OLD].peers > 0 && g_big[OLD].peers_n == RC_PEERS_V1;
    for (int i = 0; i < NP; i++) if (i != OLD && (!g_big[i].hellos || g_big[i].hello_n != NP || !g_big[i].hello_self || g_big[i].peers_n != NP)) ok_new = 0;
    CHECK(ok_new, "a group of %d: HELLOs and PEERS lists of all %d to cores of version %d (to player 0: %d ids, %d peers)", NP + 1, NP, RC_CORE_VER,
          g_big[0].hello_n, g_big[0].peers_n);
    CHECK(ok_old, "the player of core version 1: HELLOs of at most 7 ids with its own (%d, %s), PEERS of 7 (%d)",
          g_big[OLD].hello_n, g_big[OLD].hello_self ? "with it" : "WITHOUT it", g_big[OLD].peers_n);
    /* snapshots by distance: player i is i * 100 + 50 m away; 20 a second
     * within 300 m, 5 to 1000 m, 2 past that */
    int64_t t = now + 11000000;
    for (int i = 0; i < NP; i++){
        rc_msg m; memset(&m, 0, sizeof m); m.h.type = RC_MSG_SNAP; m.h.sender = 0x1000u + (uint32_t)i; m.h.seq = 1;
        m.u.snap.status = RC_ST_ROAM; m.u.snap.n = 1; m.u.snap.s[0].q[3] = 1; m.u.snap.s[0].p[0] = (float)(i * 100 + 50);
        uint8_t b[RC_MAX_DGRAM]; int n = rc_encode(&m, b, sizeof b);
        rc_receive(&s, 0x7f000001u, (uint16_t)(51000 + i), b, n, t);
    }
    memset(g_big, 0, sizeof g_big);
    for (int64_t u = t; u < t + 2000000; u += 10000){
        if ((u - t) % 1000000 == 0) big_hellos(&s, u);
        rc_state st; memset(&st, 0, sizeof st); st.q[3] = 1; st.t_us = (uint32_t)u;
        rc_set_status(&s, RC_ST_ROAM); rc_push_state(&s, &st, u);
        rc_tick(&s, u);
    }
    int ok_rate = 1;
    for (int i = 0; i < NP; i++){
        int d = i * 100 + 50, want = d > 1000 ? 4 : d > 300 ? 10 : 40, every = d > 1000 ? 10 : d > 300 ? 4 : 0;
        if (g_big[i].snaps < want - 1 || g_big[i].snaps > want + 1 || g_big[i].every != every) ok_rate = 0;
    }
    CHECK(ok_rate, "snapshots in 2 s by distance: %d at 50 m, %d at 450 m, %d at 1950 m (40, 10, 4), header flags %d %d %d",
          g_big[0].snaps, g_big[4].snaps, g_big[19].snaps, g_big[0].every, g_big[4].every, g_big[19].every);
    /* the beacon of a player whose group is past 8 names 7 of its ids
     * (RC_HF_ALL): ours not among them says nothing; a HELLO to us without
     * our id says it no longer hears us */
    int before = rc_established(&s);
    for (int pass = 0; pass < 2; pass++){
        rc_msg m; memset(&m, 0, sizeof m); m.h.type = RC_MSG_HELLO; m.h.sender = 0x1000u; m.h.flags = pass ? 0 : RC_HF_ALL;
        m.u.hello.core_ver = RC_CORE_VER; m.u.hello.status = RC_ST_ROAM; m.u.hello.listen_port = 51000;
        snprintf(m.u.hello.name, sizeof m.u.hello.name, "p0"); m.u.hello.n_seen = RC_PEERS_V1;
        for (int k = 0; k < RC_PEERS_V1; k++) m.u.hello.seen[k] = 0x1001u + (uint32_t)k;
        uint8_t b[RC_MAX_DGRAM]; int n = rc_encode(&m, b, sizeof b);
        rc_receive(&s, 0x7f000001u, 51000, b, n, t + 2000000 + pass * 1000);
        if (!pass) CHECK(rc_established(&s) == before, "a beacon of 7 ids without ours: the player stays (%d of %d)", rc_established(&s), before);
        else CHECK(rc_established(&s) == before - 1, "a HELLO to us without our id: the player left (%d of %d)", rc_established(&s), before);
    }
}

/* ---- 4d. outruns between players: DUEL only to a peer that takes them, only for us -- */
static void t_duel(void){
    mesh(3, 8);
    g_n[0].conf.duels = 1; g_n[1].conf.duels = 1;             /* node 2: an older game */
    for (int i = 1; i < 3; i++){ rc_add_peer(&g_n[0], node_ip(i), node_port(i), 1, g_now); rc_add_peer(&g_n[i], node_ip(0), node_port(0), 1, g_now); }
    drive(3, g_now + 3000000);
    int p1 = peer_of(0, 0x501), p2 = peer_of(0, 0x502);
    CHECK(est(0) == 2 && p1 >= 0 && p2 >= 0 && g_n[0].peer[p1].duels && !g_n[0].peer[p2].duels,
          "the HELLO's flag says who takes outrun challenges (%d %d)", p1 >= 0 && g_n[0].peer[p1].duels, p2 >= 0 && g_n[0].peer[p2].duels);
    uint32_t bad2 = g_n[2].rx_bad;
    CHECK(rc_send_duel(&g_n[0], 0x501, RC_DUEL_OFFER, 0, 0xD1) == 1 && rc_send_duel(&g_n[0], 0x502, RC_DUEL_OFFER, 0, 0xD2) == 0 &&
          rc_send_duel(&g_n[0], 0x777, RC_DUEL_OFFER, 0, 0xD3) == 0, "DUEL goes to a peer that takes it, not to an older one or a stranger");
    net_run(g_now + 20000, 3);
    uint32_t from = 0, duel = 0; uint8_t op = 0, arg = 0;
    CHECK(rc_take_duel(&g_n[1], &from, &op, &arg, &duel) == 1 && from == 0x500 && op == RC_DUEL_OFFER && duel == 0xD1 &&
          !rc_take_duel(&g_n[1], &from, &op, &arg, &duel), "the challenge arrives once (from %x, op %d, id %x)", from, op, duel);
    CHECK(g_n[2].rx_bad == bad2 && !g_n[2].n_duel_in, "the older game hears nothing it cannot read");
    /* one for somebody else, from an established peer: not ours */
    rc_msg m; memset(&m, 0, sizeof m); m.h.type = RC_MSG_DUEL; m.h.sender = 0x500;
    m.u.duel.op = RC_DUEL_ACCEPT; m.u.duel.to = 0x999; m.u.duel.duel = 0xD1;
    uint8_t b[RC_MAX_DGRAM]; int n = rc_encode(&m, b, sizeof b);
    rc_receive(&g_n[1], node_ip(0), node_port(0), b, n, g_now);
    CHECK(!g_n[1].n_duel_in, "a DUEL for another player is dropped");
    /* the inbox holds RC_DUEL_INBOX, oldest first */
    for (int k = 0; k < RC_DUEL_INBOX + 4; k++) rc_send_duel(&g_n[0], 0x501, RC_DUEL_RESULT, 1, 0x100u + (uint32_t)k);
    net_run(g_now + 20000, 3);
    int kept = g_n[1].n_duel_in, taken = 0; uint32_t seen = 0;
    while (rc_take_duel(&g_n[1], &from, &op, &arg, &duel)) if (duel >= 0x100 && duel < 0x100 + RC_DUEL_INBOX + 4){ taken++; seen |= 1u << (duel - 0x100); }
    CHECK(kept == RC_DUEL_INBOX && g_n[1].duel_dropped == 4 && taken == RC_DUEL_INBOX && __builtin_popcount(seen) == RC_DUEL_INBOX && !g_n[1].n_duel_in,
          "inbox: %d kept, %u dropped, %d taken", kept, g_n[1].duel_dropped, taken);
}

/* ---- 4f. state extensions: wheel heights, nitrous, hydraulics, only to a peer that reads them -- */
static void t_statex(void){
    /* layout: a length byte, 4 x int16 mm, the nitrous byte, the hydraulics and landings bytes; 0 = none */
    uint8_t b[RC_MAX_DGRAM];
    rc_msg m; memset(&m, 0, sizeof m);
    m.h.type = RC_MSG_SNAP; m.h.sender = 5; m.h.flags = RC_HF_STATEX; m.u.snap.status = RC_ST_ROAM; m.u.snap.n = 2;
    for (int k = 0; k < 2; k++){ m.u.snap.s[k].q[3] = 1; m.u.snap.s[k].t_us = 1000u - (uint32_t)k; }
    rc_state *s = &m.u.snap.s[0];
    s->ext = 1; s->wz[0] = 0.187f; s->wz[1] = -0.0355f; s->wz[2] = 40.f; s->wz[3] = -0.0004f; s->nos = 150; s->hyd = 0x9c; s->thumps = 0xfe;
    int n = rc_encode(&m, b, sizeof b);
    CHECK(n == 16 + 6 + 2 * 52 + 12 + 1 + 4, "STATEX SNAP length %d (one state with its extension, one without)", n);
    const uint8_t *x = b + 22 + 52;
    CHECK(x[0] == 11 && (int16_t)le16(x + 1) == 187 && (int16_t)le16(x + 3) == -36 && (int16_t)le16(x + 5) == 32000 &&
          (int16_t)le16(x + 7) == 0 && x[9] == 150 && x[10] == 0x9c && x[11] == 0xfe && x[12 + 52] == 0,
          "STATEX layout (mm, clamped to 32 m, nitrous, hydraulics, landings, 0 for none)");
    rc_msg d; int e = rc_decode(b, n, &d);
    CHECK(e == RC_OK && d.u.snap.s[0].ext && fabsf(d.u.snap.s[0].wz[0] - 0.187f) < 1e-6f && fabsf(d.u.snap.s[0].wz[1] + 0.036f) < 1e-6f &&
          d.u.snap.s[0].nos == 150 && d.u.snap.s[0].hyd == 0x9c && d.u.snap.s[0].thumps == 0xfe &&
          !d.u.snap.s[1].ext && d.u.snap.s[1].nos == 0 && !d.u.snap.s[1].hyd && !d.u.snap.s[1].thumps, "STATEX decodes (e %d)", e);
    /* the first form of version 4 (9 bytes: no hydraulics) */
    uint8_t ob[RC_MAX_DGRAM]; memcpy(ob, b, 22 + 52 + 10);
    ob[22 + 52] = 9; memcpy(ob + 22 + 52 + 10, b + 22 + 52 + 12, (size_t)(n - 4 - (22 + 52 + 12)));
    int on = n - 2; ob[6] = (uint8_t)on; ob[7] = (uint8_t)(on >> 8);
    uint32_t oc = rc_crc32(ob, (uint32_t)on - 4); for (int k = 0; k < 4; k++) ob[on - 4 + k] = (uint8_t)(oc >> (8 * k));
    e = rc_decode(ob, on, &d);
    CHECK(e == RC_OK && d.u.snap.s[0].ext && d.u.snap.s[0].nos == 150 && fabsf(d.u.snap.s[0].wz[0] - 0.187f) < 1e-6f &&
          !d.u.snap.s[0].hyd && !d.u.snap.s[0].thumps && !d.u.snap.s[1].ext, "the first version 4 form decodes, without hydraulics (e %d)", e);
    /* the wheels' slip: only while a wheel slides (half a step of slip or more) */
    memset(&m, 0, sizeof m); m.h.type = RC_MSG_SNAP; m.h.sender = 5; m.h.flags = RC_HF_STATEX; m.u.snap.status = RC_ST_ROAM; m.u.snap.n = 1;
    s = &m.u.snap.s[0]; s->q[3] = 1; s->ext = 1; s->nos = 7; s->wz[2] = 0.25f;
    s->slip[0] = 0.24f; s->slip[3] = -0.2f; s->surf[0] = 1; s->surf[2] = 8;
    n = rc_encode(&m, b, sizeof b);
    CHECK(n == 16 + 6 + 52 + 12 + 4 && b[22 + 52] == RC_STATEX_HYD - 1, "no wheel slides: the extension without slip (%d bytes)", n);
    s->slip[4] = 21.7f; s->slip[5] = -6.3f; s->slip[6] = 500.f; s->slip[7] = __builtin_nanf(""); s->slip[1] = -63.4f;
    n = rc_encode(&m, b, sizeof b);
    x = b + 22 + 52;
    CHECK(n == 16 + 6 + 52 + 24 + 4 && x[0] == 23 && (int8_t)x[12] == 0 && (int8_t)x[13] == -127 && x[14] == 1 && (int8_t)x[15] == 0 &&
          (int8_t)x[16] == 0 && (int8_t)x[18] == 43 && (int8_t)x[19] == -13 && x[20] == 8 && (int8_t)x[21] == 127 && (int8_t)x[22] == 0,
          "a wheel slides: each wheel's slip in half m/s (clamped, NaN 0) and its surface (%d bytes)", n);
    e = rc_decode(b, n, &d);
    CHECK(e == RC_OK && d.u.snap.s[0].slipx && d.u.snap.s[0].slip[4] == 21.5f && d.u.snap.s[0].slip[5] == -6.5f && d.u.snap.s[0].slip[6] == 63.5f &&
          d.u.snap.s[0].slip[1] == -63.5f && d.u.snap.s[0].surf[2] == 8 && d.u.snap.s[0].nos == 7 && !d.u.snap.s[0].eng, "the slip decodes (e %d)", e);
    /* the engine: after the slip while a wheel slides, after the hydraulics otherwise; none while it is 0 */
    uint8_t eb[RC_MAX_DGRAM];
    s->eng = 5948; s->eng_acc = 255; s->eng_dcl = 38; s->eng_base = 99;
    int en = rc_encode(&m, eb, sizeof eb); const uint8_t *ex = eb + 22 + 52;
    CHECK(en == 16 + 6 + 52 + 29 + 4 && ex[0] == 28 && le16(ex + 24) == 5948 && ex[26] == 255 && ex[27] == 38 && ex[28] == 99 && (int8_t)ex[18] == 43,
          "the engine after the slip (%d bytes)", en);
    e = rc_decode(eb, en, &d);
    CHECK(e == RC_OK && d.u.snap.s[0].slipx && d.u.snap.s[0].slip[4] == 21.5f && d.u.snap.s[0].eng == 5948 && d.u.snap.s[0].eng_acc == 255 &&
          d.u.snap.s[0].eng_dcl == 38 && d.u.snap.s[0].eng_base == 99, "...decodes (e %d)", e);
    rc_state keep = *s;
    for (int i = 0; i < 8; i++) s->slip[i] = 0;
    s->eng = 30000;
    en = rc_encode(&m, eb, sizeof eb);
    CHECK(en == 16 + 6 + 52 + 17 + 4 && ex[0] == 16 && le16(ex + 12) == 20000 && ex[14] == 255 && ex[15] == 38 && ex[16] == 99,
          "the engine after the hydraulics, its rpm at most 20000 (%d bytes)", en);
    e = rc_decode(eb, en, &d);
    CHECK(e == RC_OK && !d.u.snap.s[0].slipx && d.u.snap.s[0].slip[4] == 0 && d.u.snap.s[0].nos == 7 && d.u.snap.s[0].eng == 20000 &&
          d.u.snap.s[0].eng_dcl == 38, "...decodes, without slip (e %d)", e);
    /* (a core before the engine reads 16 as the hydraulics and 28 as the slip, and skips the rest: the two tests below) */
    *s = keep;
    en = rc_encode(&m, eb, sizeof eb);
    eb[22 + 52] = RC_STATEX_BYTES - 1 + 3; eb[en - 4] = 0xAA; eb[en - 3] = 0xBB; eb[en - 2] = 0xCC;
    int eln = en + 3; eb[6] = (uint8_t)eln; eb[7] = (uint8_t)(eln >> 8);
    uint32_t ec = rc_crc32(eb, (uint32_t)eln - 4); for (int k = 0; k < 4; k++) eb[eln - 4 + k] = (uint8_t)(ec >> (8 * k));
    e = rc_decode(eb, eln, &d);
    CHECK(e == RC_OK && d.u.snap.s[0].slip[4] == 21.5f && d.u.snap.s[0].eng == 5948 && d.u.snap.s[0].eng_dcl == 38,
          "a newer core's fields after the engine: skipped (e %d)", e);
    s->eng = 0; s->eng_acc = 0; s->eng_dcl = 0; s->eng_base = 0;
    /* a newer core's longer extension: its own fields skipped */
    uint8_t lb[RC_MAX_DGRAM]; memcpy(lb, b, (size_t)n - 4);
    lb[22 + 52] = RC_STATEX_SLIP - 1 + 3; lb[n - 4] = 0xAA; lb[n - 3] = 0xBB; lb[n - 2] = 0xCC;
    int ln = n + 3; lb[6] = (uint8_t)ln; lb[7] = (uint8_t)(ln >> 8);
    uint32_t c = rc_crc32(lb, (uint32_t)ln - 4); for (int k = 0; k < 4; k++) lb[ln - 4 + k] = (uint8_t)(c >> (8 * k));
    e = rc_decode(lb, ln, &d);
    CHECK(e == RC_OK && d.u.snap.s[0].ext && d.u.snap.s[0].nos == 7 && d.u.snap.s[0].wz[2] == 0.25f && d.u.snap.s[0].slip[4] == 21.5f,
          "a longer extension is read, its rest skipped (e %d)", e);
    /* what a core before the slip reads of it: the hydraulics, the rest skipped (as its own decoder did) */
    memcpy(lb, b, (size_t)(22 + 52 + 12)); lb[22 + 52 + 12] = 0xAA; lb[22 + 52 + 13] = 0xBB; lb[22 + 52 + 14] = 0xCC;
    lb[22 + 52] = RC_STATEX_HYD - 1 + 3; ln = 22 + 52 + 12 + 3 + 4; lb[6] = (uint8_t)ln; lb[7] = (uint8_t)(ln >> 8);
    c = rc_crc32(lb, (uint32_t)ln - 4); for (int k = 0; k < 4; k++) lb[ln - 4 + k] = (uint8_t)(c >> (8 * k));
    e = rc_decode(lb, ln, &d);
    CHECK(e == RC_OK && d.u.snap.s[0].ext && d.u.snap.s[0].nos == 7 && !d.u.snap.s[0].slipx && d.u.snap.s[0].slip[4] == 0,
          "between the hydraulics and the slip: no slip, the rest skipped (e %d)", e);
    lb[22 + 52] = 4; c = rc_crc32(lb, (uint32_t)ln - 4); for (int k = 0; k < 4; k++) lb[ln - 4 + k] = (uint8_t)(c >> (8 * k));
    CHECK(rc_decode(lb, ln, &d) == RC_E_FIELD, "an extension shorter than this core's fields is bad");
    m.h.flags = 0; n = rc_encode(&m, b, sizeof b);
    CHECK(n == 16 + 6 + 52 + 4 && rc_decode(b, n, &d) == RC_OK && !d.u.snap.s[0].ext && !d.u.snap.s[0].nos && d.u.snap.s[0].wz[2] == 0,
          "without the flag: the plain state, no extension (%d bytes)", n);

    /* sessions: node 2 is an older core (its HELLO without the flag), node 0 also reports to a hub */
    mesh(3, 8);
    memset(g_snapx_rx, 0, sizeof g_snapx_rx); memset(g_snap_rx, 0, sizeof g_snap_rx); memset(g_hub_snapx, 0, sizeof g_hub_snapx);
    g_nox_from = 2; g_n[0].conf.hub_ip = HUB_IP; g_n[0].conf.hub_port = HUB_PORT;
    for (int i = 1; i < 3; i++){ rc_add_peer(&g_n[0], node_ip(i), node_port(i), 1, g_now); rc_add_peer(&g_n[i], node_ip(0), node_port(0), 1, g_now); }
    int64_t until = g_now + 3000000;
    while (g_now < until){
        for (int i = 0; i < 3; i++){
            rc_state st; memset(&st, 0, sizeof st); st.q[3] = 1; st.t_us = (uint32_t)g_now; st.p[0] = (float)i;
            st.ext = 1; st.nos = 60; st.hyd = 0x36; st.thumps = 3; for (int w = 0; w < 4; w++) st.wz[w] = 0.1f * (float)(w + 1);
            st.slip[6] = 12.f; st.slip[7] = -3.5f; st.surf[3] = 15;
            st.eng = 4000; st.eng_acc = 255; st.eng_dcl = 51; st.eng_base = 102;
            rc_set_status(&g_n[i], RC_ST_ROAM); rc_push_state(&g_n[i], &st, g_now);
        }
        net_run(g_now + 10000, 3);
    }
    g_nox_from = -1; g_n[0].conf.hub_ip = 0;
    int p1 = peer_of(0, 0x501), p2 = peer_of(0, 0x502), k1 = peer_of(1, 0x500), k2 = peer_of(2, 0x500);
    CHECK(est(0) == 2 && p1 >= 0 && p2 >= 0 && g_n[0].peer[p1].statex && !g_n[0].peer[p2].statex,
          "the HELLO's flag says who reads state extensions");
    CHECK(g_snapx_rx[1] > 20 && g_snap_rx[2] > 20 && !g_snapx_rx[2] && g_n[2].rx_bad == 0,
          "extended SNAPs to the newer peer (%d), plain ones to the older (%d plain, %d extended, %u bad)",
          g_snapx_rx[1], g_snap_rx[2], g_snapx_rx[2], g_n[2].rx_bad);
    CHECK(g_hub_snap[0] > 0 && !g_hub_snapx[0], "the hub gets plain SNAPs (%d, %d extended)", g_hub_snap[0], g_hub_snapx[0]);
    rc_pose P;
    int ok1 = k1 >= 0 && rc_interp_peek(&g_n[1].peer[k1].mo, g_n[1].peer[k1].mo.t[g_n[1].peer[k1].mo.n - 1], 250000, &P);
    CHECK(ok1 && P.ext && P.nos == 60 && P.hyd == 0x36 && P.thumps == 3 && fabsf(P.wz[0] - 0.1f) < 1e-6f && fabsf(P.wz[3] - 0.4f) < 1e-6f,
          "the newer peer's pose has the wheel heights, the nitrous and the hydraulics (%d %d %x %u %g)", P.ext, P.nos, P.hyd, P.thumps, (double)P.wz[3]);
    CHECK(ok1 && P.slipx && P.slip[6] == 12.f && P.slip[7] == -3.5f && P.surf[3] == 15 && P.slip[0] == 0,
          "...and the wheels' slip and surfaces (%d %g %g %u)", P.slipx, (double)P.slip[6], (double)P.slip[7], P.surf[3]);
    CHECK(ok1 && P.eng == 4000.f && P.eng_acc == 1.f && fabsf(P.eng_dcl - 0.2f) < 1e-6f && fabsf(P.eng_base - 0.4f) < 1e-6f,
          "...and its engine (%g %g %g %g)", (double)P.eng, (double)P.eng_acc, (double)P.eng_dcl, (double)P.eng_base);
    int ok2 = k2 >= 0 && rc_interp_peek(&g_n[2].peer[k2].mo, g_n[2].peer[k2].mo.t[g_n[2].peer[k2].mo.n - 1], 250000, &P);
    CHECK(ok2 && !P.ext && !P.nos && !P.hyd && !P.thumps && !P.slipx && P.slip[6] == 0 && !P.eng, "the older peer's pose has none");
}

/* ---- 4f. crews: the HELLO's tail to the hub, and without a hub to a peer that reads them; the hub's marks ---- */
static void t_crews(void){
    uint8_t b[RC_MAX_DGRAM]; rc_msg m, d; memset(&m, 0, sizeof m);
    uint32_t A = rc_room_hash("Blue Room"), B = rc_room_hash("Night Owls");
    m.h.type = RC_MSG_HELLO; m.h.sender = 7; m.h.flags = RC_HF_CREW; strcpy(m.u.hello.name, "Thom");
    m.u.hello.n_seen = 1; m.u.hello.seen[0] = 0x11111111u;
    int base = 16 + 2 + 1 + 1 + 2 + 2 + 4 + 1 + 4 + 1 + 4 + 4;
    int n = rc_encode(&m, b, sizeof b);
    CHECK(n == base && rc_decode(b, n, &d) == RC_OK && d.u.hello.n_crews == 0, "HELLO with the flag and no crews: as before (%d bytes)", n);
    m.u.hello.n_crews = 2; m.u.hello.crews[0] = A; m.u.hello.crews[1] = B;
    n = rc_encode(&m, b, sizeof b);
    CHECK(n == base + 1 + 8 && b[n - 4 - 9] == 2 && le32(b + n - 4 - 8) == A && le32(b + n - 4 - 4) == B, "HELLO's crews after the ids (%d bytes)", n);
    CHECK(rc_decode(b, n, &d) == RC_OK && d.u.hello.n_crews == 2 && d.u.hello.crews[0] == A && d.u.hello.crews[1] == B && d.u.hello.n_seen == 1 &&
          d.u.hello.seen[0] == 0x11111111u, "HELLO's crews round trip");
    /* what a newer core adds after them is skipped */
    uint8_t lb[RC_MAX_DGRAM]; memcpy(lb, b, (size_t)n - 4); lb[n - 4] = 0xAA; lb[n - 3] = 0xBB; lb[n - 2] = 0xCC;
    int ln = n + 3; lb[6] = (uint8_t)ln; lb[7] = (uint8_t)(ln >> 8);
    uint32_t c = rc_crc32(lb, (uint32_t)ln - 4); for (int k = 0; k < 4; k++) lb[ln - 4 + k] = (uint8_t)(c >> (8 * k));
    CHECK(rc_decode(lb, ln, &d) == RC_OK && d.u.hello.n_crews == 2 && d.u.hello.crews[1] == B, "bytes after the crews skipped");
    /* without the flag the same bytes are trailing, as an older core takes them */
    lb[15] &= (uint8_t)~(RC_HF_CREW >> 8); c = rc_crc32(lb, (uint32_t)ln - 4); for (int k = 0; k < 4; k++) lb[ln - 4 + k] = (uint8_t)(c >> (8 * k));
    CHECK(rc_decode(lb, ln, &d) == RC_E_TRAIL, "without the flag: trailing bytes are bad");
    memcpy(lb, b, (size_t)n); lb[n - 4 - 9] = RC_CREW_MAX + 1; c = rc_crc32(lb, (uint32_t)n - 4); for (int k = 0; k < 4; k++) lb[n - 4 + k] = (uint8_t)(c >> (8 * k));
    CHECK(rc_decode(lb, n, &d) == RC_E_FIELD || rc_decode(lb, n, &d) == RC_E_SHORT, "more than RC_CREW_MAX crews is bad");
    m.u.hello.crews[1] = 0; CHECK(rc_encode(&m, b, sizeof b) < 0, "a crew 0 is not sent");
    memcpy(lb, b, (size_t)n); m.u.hello.crews[1] = B;
    n = rc_encode(&m, b, sizeof b); memcpy(lb, b, (size_t)n); memset(lb + n - 8, 0, 4);
    c = rc_crc32(lb, (uint32_t)n - 4); for (int k = 0; k < 4; k++) lb[n - 4 + k] = (uint8_t)(c >> (8 * k));
    CHECK(rc_decode(lb, n, &d) == RC_E_FIELD, "a crew 0 heard is bad");
    m.h.flags = 0; CHECK(rc_encode(&m, b, sizeof b) < 0, "crews without the flag are not sent");

    /* the hub's PEERS with crew marks (RC_HF_CREW on a PEERS): the receiver's crews, then a byte per entry */
    memset(&m, 0, sizeof m); m.h.type = RC_MSG_PEERS; m.h.sender = 0xB0B; m.h.flags = RC_HF_CREW;
    m.u.peers.n = 3;
    for (int k = 0; k < 3; k++){ m.u.peers.e[k].ip = 0x0A4D0010u + (uint32_t)k; m.u.peers.e[k].port = 47210; m.u.peers.e[k].id = 0x20u + (uint32_t)k; }
    m.u.peers.n_mine = 2; m.u.peers.mine[0] = A; m.u.peers.mine[1] = B; m.u.peers.crew[0] = 2; m.u.peers.crew[1] = 0; m.u.peers.crew[2] = 3;
    n = rc_encode(&m, b, sizeof b);
    int pbase = 16 + 1 + 3 * 10 + 4;
    CHECK(n == pbase + 1 + 8 + 3 && b[pbase - 4] == 2 && le32(b + pbase - 3) == A && le32(b + pbase + 1) == B && b[n - 7] == 2 && b[n - 5] == 3,
          "PEERS: the receiver's crews and a mark an entry after the entries (%d bytes)", n);
    CHECK(rc_decode(b, n, &d) == RC_OK && d.u.peers.n == 3 && d.u.peers.n_mine == 2 && d.u.peers.mine[0] == A && d.u.peers.mine[1] == B &&
          d.u.peers.crew[0] == 2 && d.u.peers.crew[1] == 0 && d.u.peers.crew[2] == 3 && d.u.peers.e[2].id == 0x22u, "PEERS' crew marks round trip");
    memcpy(lb, b, (size_t)n - 4); lb[n - 4] = 0xAA; lb[n - 3] = 0xBB; ln = n + 2; lb[6] = (uint8_t)ln; lb[7] = (uint8_t)(ln >> 8);
    c = rc_crc32(lb, (uint32_t)ln - 4); for (int k = 0; k < 4; k++) lb[ln - 4 + k] = (uint8_t)(c >> (8 * k));
    CHECK(rc_decode(lb, ln, &d) == RC_OK && d.u.peers.crew[2] == 3, "bytes after the marks skipped (a newer hub's)");
    lb[15] &= (uint8_t)~(RC_HF_CREW >> 8); c = rc_crc32(lb, (uint32_t)ln - 4); for (int k = 0; k < 4; k++) lb[ln - 4 + k] = (uint8_t)(c >> (8 * k));
    CHECK(rc_decode(lb, ln, &d) == RC_E_TRAIL, "without the flag the marks are trailing bytes: an older core gets none");
    memcpy(lb, b, (size_t)n); lb[pbase - 4] = RC_CREW_MAX + 1; c = rc_crc32(lb, (uint32_t)n - 4); for (int k = 0; k < 4; k++) lb[n - 4 + k] = (uint8_t)(c >> (8 * k));
    CHECK(rc_decode(lb, n, &d) == RC_E_FIELD, "more than RC_CREW_MAX crews of the receiver is bad");
    memcpy(lb, b, (size_t)n); memset(lb + pbase + 1, 0, 4); c = rc_crc32(lb, (uint32_t)n - 4); for (int k = 0; k < 4; k++) lb[n - 4 + k] = (uint8_t)(c >> (8 * k));
    CHECK(rc_decode(lb, n, &d) == RC_E_FIELD, "a crew 0 in the marks is bad");
    memcpy(lb, b, (size_t)n - 5); ln = n - 1; lb[6] = (uint8_t)ln; lb[7] = (uint8_t)(ln >> 8);
    c = rc_crc32(lb, (uint32_t)ln - 4); for (int k = 0; k < 4; k++) lb[ln - 4 + k] = (uint8_t)(c >> (8 * k));
    CHECK(rc_decode(lb, ln, &d) == RC_E_SHORT, "marks cut short are bad");
    m.u.peers.n_mine = 0; n = rc_encode(&m, b, sizeof b);
    CHECK(n == pbase + 1 + 3 && rc_decode(b, n, &d) == RC_OK && d.u.peers.n_mine == 0, "the flag and no crews of the receiver: marks of nothing");
    m.h.flags = 0; n = rc_encode(&m, b, sizeof b);
    CHECK(n == pbase && rc_decode(b, n, &d) == RC_OK && d.u.peers.n_mine == 0 && !d.u.peers.crew[0], "without the flag: a list of before");

    /* sessions without a hub (a LAN of its own): 0 in crews A and B, 1 in B, 2 an older core (its HELLO without the
     * flag); the crews go in the HELLOs */
    mesh(3, 8);
    g_n[0].conf.crews[0] = A; g_n[0].conf.crews[1] = B; g_n[0].conf.n_crews = 2;
    g_n[1].conf.crews[0] = B; g_n[1].conf.n_crews = 1;
    g_nocrew_from = 2; g_crew_to_old = 0; memset(g_hub_ncrews, 0, sizeof g_hub_ncrews);
    for (int i = 1; i < 3; i++){ rc_add_peer(&g_n[0], node_ip(i), node_port(i), 1, g_now); rc_add_peer(&g_n[i], node_ip(0), node_port(0), 1, g_now); }
    drive(3, g_now + 3000000);
    g_nocrew_from = -1;
    int p1 = peer_of(0, 0x501), p2 = peer_of(0, 0x502), k0 = peer_of(1, 0x500), j0 = peer_of(2, 0x500);
    CHECK(est(0) == 2 && est(1) >= 1 && est(2) >= 1, "crews or none, everyone sees everyone (%d %d %d)", est(0), est(1), est(2));
    CHECK(p1 >= 0 && rc_peer_crew(&g_n[0], p1) == 1 && k0 >= 0 && rc_peer_crew(&g_n[1], k0) == 0,
          "a shared crew, as each one's own crews number it (%d, %d)", p1 >= 0 ? rc_peer_crew(&g_n[0], p1) : -9, k0 >= 0 ? rc_peer_crew(&g_n[1], k0) : -9);
    CHECK(p2 >= 0 && rc_peer_crew(&g_n[0], p2) < 0 && !g_n[0].peer[p2].reads_crews && j0 >= 0 && rc_peer_crew(&g_n[2], j0) < 0,
          "the older core is in no crew");
    CHECK(!g_crew_to_old && !g_n[2].rx_bad, "no crews to the older core (%d HELLOs, %u bad)", g_crew_to_old, g_n[2].rx_bad);

    /* crews changed while the sessions run (the page's menu): the others hear them within a tick or two */
    uint32_t none[1] = { 0 }, two[3] = { B, 0, A };
    rc_set_crews(&g_n[1], none, 1);
    drive(3, g_now + 100000);
    CHECK(rc_peer_crew(&g_n[0], p1) < 0 && g_n[1].conf.n_crews == 0, "node 1 left its crews: no longer in node 0's (%d)", rc_peer_crew(&g_n[0], p1));
    rc_set_crews(&g_n[1], two, 3);
    rc_set_crews(&g_n[0], two + 2, 1);
    int64_t t0 = g_now;
    drive(3, g_now + 100000);
    CHECK(g_n[1].conf.n_crews == 2 && g_n[1].conf.crews[0] == B && g_n[1].conf.crews[1] == A, "0s left out (%d crews)", g_n[1].conf.n_crews);
    CHECK(rc_peer_crew(&g_n[0], p1) == 0 && rc_peer_crew(&g_n[1], k0) == 1, "...and joined again, as each one's crews number them now (%d, %d)",
          rc_peer_crew(&g_n[0], p1), rc_peer_crew(&g_n[1], k0));
    (void)t0;

    /* with the server's hub: 0 in A and B, 1 in B, 2 in none.  The crews go to the hub only, never from one node
     * to another, and each node learns from the hub's marks which of its peers share one of its crews */
    mesh(3, 8);
    g_n[0].conf.crews[0] = A; g_n[0].conf.crews[1] = B; g_n[0].conf.n_crews = 2;
    g_n[1].conf.crews[0] = B; g_n[1].conf.n_crews = 1;
    g_hub_on = 1; g_hub_next = 0; memset(g_hub_link, 0, sizeof g_hub_link);
    memset(g_hub_ncrews, 0, sizeof g_hub_ncrews); memset(g_hub_flags, 0, sizeof g_hub_flags);
    for (int i = 0; i < 3; i++){ g_n[i].conf.hub_ip = HUB_IP; g_n[i].conf.hub_port = HUB_PORT; }
    hub_links(0, 1); hub_links(0, 2); hub_links(1, 2);
    g_crew_p2p = 0;
    drive(3, g_now + 4000000);
    p1 = peer_of(0, 0x501); p2 = peer_of(0, 0x502); k0 = peer_of(1, 0x500); j0 = peer_of(2, 0x500);
    int k2 = peer_of(1, 0x502);
    CHECK(est(0) == 2 && est(1) == 2 && est(2) == 2, "hub: everyone it lists established (%d %d %d)", est(0), est(1), est(2));
    CHECK((g_hub_flags[0] & RC_HF_HUBCREW) && g_hub_ncrews[0] == 2 && g_hub_crews[0][1] == B && g_hub_ncrews[1] == 1 && g_hub_ncrews[2] == 0,
          "hub: it gets each one's crews, and that it reads the marks");
    CHECK(!g_crew_p2p, "hub: no HELLO from one node to another names a crew (%d)", g_crew_p2p);
    CHECK(p1 >= 0 && k0 >= 0 && !g_n[0].peer[p1].n_crews && !g_n[1].peer[k0].n_crews, "hub: no node knows another's crews");
    CHECK(rc_peer_crew(&g_n[0], p1) == 1 && rc_peer_crew(&g_n[1], k0) == 0, "hub: the shared crew from its marks, as each one's crews number it (%d, %d)",
          rc_peer_crew(&g_n[0], p1), rc_peer_crew(&g_n[1], k0));
    CHECK(p2 >= 0 && rc_peer_crew(&g_n[0], p2) < 0 && k2 >= 0 && rc_peer_crew(&g_n[1], k2) < 0 && j0 >= 0 && rc_peer_crew(&g_n[2], j0) < 0,
          "hub: a node in no crew is in none of theirs, and sees none");
    /* node 1 leaves its crew, then joins A and B again in the other order: the hub's next list (1 s) says so */
    rc_set_crews(&g_n[1], none, 1);
    drive(3, g_now + 1500000);
    CHECK(rc_peer_crew(&g_n[0], p1) < 0 && rc_peer_crew(&g_n[1], k0) < 0, "hub: node 1 left: no longer in node 0's crews (%d)", rc_peer_crew(&g_n[0], p1));
    uint32_t ba[2] = { B, A };
    rc_set_crews(&g_n[1], ba, 2);
    drive(3, g_now + 1500000);
    CHECK(rc_peer_crew(&g_n[0], p1) == 0 && rc_peer_crew(&g_n[1], k0) == 0, "hub: joined again: in A, node 0's first, and in B, node 1's first (%d, %d)",
          rc_peer_crew(&g_n[0], p1), rc_peer_crew(&g_n[1], k0));
    CHECK(!g_crew_p2p, "hub: still no crews between the nodes (%d)", g_crew_p2p);
    g_hub_on = 0; for (int i = 0; i < 3; i++) g_n[i].conf.hub_ip = 0;
    memset(g_hub_ncrews, 0, sizeof g_hub_ncrews); memset(g_hub_flags, 0, sizeof g_hub_flags);
}

/* ---- 4e. race lobbies: LOBBY only to a peer that takes them, only for us; the hub's LOBBIES -- */
static void t_lobby(void){
    mesh(3, 8);
    g_n[0].conf.lobbies = 1; g_n[1].conf.lobbies = 1;          /* node 2: an older game */
    for (int i = 1; i < 3; i++){ rc_add_peer(&g_n[0], node_ip(i), node_port(i), 1, g_now); rc_add_peer(&g_n[i], node_ip(0), node_port(0), 1, g_now); }
    drive(3, g_now + 3000000);
    int p1 = peer_of(0, 0x501), p2 = peer_of(0, 0x502);
    CHECK(est(0) == 2 && p1 >= 0 && p2 >= 0 && g_n[0].peer[p1].lobbies && !g_n[0].peer[p2].lobbies,
          "the HELLO's flag says who takes race lobbies (%d %d)", p1 >= 0 && g_n[0].peer[p1].lobbies, p2 >= 0 && g_n[0].peer[p2].lobbies);
    rc_lobby l; memset(&l, 0, sizeof l);
    l.op = RC_LOBBY_ADVERT; l.max = 4; l.lobby = 0xAB; l.race = 0x1234; l.x = 100; l.y = -200; l.n = 1; l.ids[0] = 0x500;
    uint32_t bad2 = g_n[2].rx_bad;
    l.to = 0x501; int a = rc_send_lobby(&g_n[0], &l);
    l.to = 0x502; int b2 = rc_send_lobby(&g_n[0], &l);
    l.to = 0x777; int c = rc_send_lobby(&g_n[0], &l);
    l.to = 0;     int h = rc_send_lobby(&g_n[0], &l);
    CHECK(a == 1 && !b2 && !c && !h, "LOBBY goes to a peer that takes it, not to an older one, a stranger or a hub that is not there");
    net_run(g_now + 20000, 3);
    uint32_t from = 0; rc_lobby got; memset(&got, 0, sizeof got);
    CHECK(rc_take_lobby(&g_n[1], &from, &got) == 1 && from == 0x500 && got.op == RC_LOBBY_ADVERT && got.lobby == 0xAB && got.race == 0x1234 &&
          got.x == 100 && got.y == -200 && got.n == 1 && got.ids[0] == 0x500 && !rc_take_lobby(&g_n[1], &from, &got),
          "the lobby arrives once (from %x, op %d, lobby %x)", from, got.op, got.lobby);
    CHECK(g_n[2].rx_bad == bad2 && !g_n[2].n_lobby_in, "the older game hears nothing it cannot read");
    /* one for somebody else, or for a game that takes none: dropped */
    rc_msg m; memset(&m, 0, sizeof m); m.h.type = RC_MSG_LOBBY; m.h.sender = 0x500;
    m.u.lobby = l; m.u.lobby.to = 0x999;
    uint8_t b[RC_MAX_DGRAM]; int n = rc_encode(&m, b, sizeof b);
    rc_receive(&g_n[1], node_ip(0), node_port(0), b, n, g_now);
    CHECK(!g_n[1].n_lobby_in, "a LOBBY for another player is dropped");
    m.u.lobby.to = 0x502; n = rc_encode(&m, b, sizeof b);
    rc_receive(&g_n[2], node_ip(0), node_port(0), b, n, g_now);
    CHECK(!g_n[2].n_lobby_in, "a game that takes no lobbies keeps none");
    /* the inbox holds RC_LOBBY_INBOX, oldest first */
    l.to = 0x501;
    for (int k = 0; k < RC_LOBBY_INBOX + 4; k++){ l.lobby = 0x100u + (uint32_t)k; rc_send_lobby(&g_n[0], &l); }
    net_run(g_now + 20000, 3);
    int kept = g_n[1].n_lobby_in, taken = 0; uint32_t seen = 0;
    while (rc_take_lobby(&g_n[1], &from, &got)) if (got.lobby >= 0x100 && got.lobby < 0x100 + RC_LOBBY_INBOX + 4){ taken++; seen |= 1u << (got.lobby - 0x100); }
    CHECK(kept == RC_LOBBY_INBOX && g_n[1].lobby_dropped == 4 && taken == RC_LOBBY_INBOX && __builtin_popcount(seen) == RC_LOBBY_INBOX,
          "lobby inbox: %d kept, %u dropped, %d taken", kept, g_n[1].lobby_dropped, taken);
    /* the hub: the advert goes to it while it answers, and its LOBBIES (the answer to our map question) are read */
    g_hub_on = 1; g_hub_next = 0; memset(g_hub_link, 0, sizeof g_hub_link); memset(g_hub_lobby, 0, sizeof g_hub_lobby);
    for (int i = 0; i < 3; i++){ g_n[i].conf.hub_ip = HUB_IP; g_n[i].conf.hub_port = HUB_PORT; }
    hub_links(0, 1); hub_links(0, 2);
    drive(3, g_now + 2000000);
    l.to = 0; l.lobby = 0xAB;
    CHECK(rc_send_lobby(&g_n[0], &l) == 1 && g_hub_lobby[0] == 1 && g_hub_lobby_last[0].race == 0x1234 && g_hub_lobby_last[0].x == 100,
          "the advert reaches the hub (%d)", g_hub_lobby[0]);
    CHECK(rc_ask_map(&g_n[0], g_now) == 1, "the map is asked for");
    static const uint8_t le[3 * RC_LOBBIES_ENTRY] = { 10, 0, 20, 0, 0x34, 0x12, 0, 0, 2, 6, RC_LS_OPEN,
                                                      1, 0, 1, 0, 0, 0, 0, 0, 1, 4, 0,              /* no race: skipped */
                                                      0xf6, 0xff, 5, 0, 1, 0, 0, 0, 4, 4, RC_LS_STARTING };
    memset(&m, 0, sizeof m); m.h.type = RC_MSG_LOBBIES; m.h.sender = 0xB0B; m.u.lobbies.req = (uint16_t)(g_n[0].map_req + 1);
    m.u.lobbies.n = 3; m.u.lobbies.data = le; n = rc_encode(&m, b, sizeof b);
    rc_receive(&g_n[0], HUB_IP, HUB_PORT, b, n, g_now);
    CHECK(g_n[0].lobbies_gen == 0, "LOBBIES to another question are ignored");
    m.u.lobbies.req = g_n[0].map_req; n = rc_encode(&m, b, sizeof b);
    rc_receive(&g_n[0], node_ip(1), node_port(1), b, n, g_now);
    CHECK(g_n[0].lobbies_gen == 0, "LOBBIES from a player, not the hub, are ignored");
    rc_receive(&g_n[0], HUB_IP, HUB_PORT, b, n, g_now);
    CHECK(g_n[0].lobbies_gen == 1 && g_n[0].lobbies_n == 2 && g_n[0].lobbies_e[0].x == 10 && g_n[0].lobbies_e[0].race == 0x1234 &&
          g_n[0].lobbies_e[1].x == -10 && g_n[0].lobbies_e[1].state == RC_LS_STARTING && g_n[0].lobbies_e[1].n == 4,
          "the hub's lobbies are read, a bad entry skipped (%d of 3)", g_n[0].lobbies_n);
    g_hub_on = 0; for (int i = 0; i < 3; i++) g_n[i].conf.hub_ip = 0;
}
/* ---- 4f. quick chat: SAY to every peer that takes it, never to an older game; the inbox -------- */
static void t_say(void){
    mesh(3, 8);
    g_n[0].conf.say = 1; g_n[1].conf.say = 1;                  /* node 2: an older game */
    for (int i = 1; i < 3; i++){ rc_add_peer(&g_n[0], node_ip(i), node_port(i), 1, g_now); rc_add_peer(&g_n[i], node_ip(0), node_port(0), 1, g_now); }
    drive(3, g_now + 3000000);
    int p1 = peer_of(0, 0x501), p2 = peer_of(0, 0x502);
    CHECK(est(0) == 2 && p1 >= 0 && p2 >= 0 && g_n[0].peer[p1].say && !g_n[0].peer[p2].say,
          "the HELLO's flag says who takes quick chat (%d %d)", p1 >= 0 && g_n[0].peer[p1].say, p2 >= 0 && g_n[0].peer[p2].say);
    uint32_t bad2 = g_n[2].rx_bad;
    CHECK(rc_send_say(&g_n[0], 7, 1) == 1, "SAY goes to the one peer that takes it");
    net_run(g_now + 20000, 3);
    uint32_t from = 0; uint8_t ph = 0, sq = 0;
    CHECK(rc_take_say(&g_n[1], &from, &ph, &sq) == 1 && from == 0x500 && ph == 7 && sq == 1 && !rc_take_say(&g_n[1], &from, &ph, &sq),
          "the phrase arrives once (from %x, phrase %d, seq %d)", from, ph, sq);
    CHECK(g_n[2].rx_bad == bad2 && !g_n[2].n_say_in, "the older game hears nothing it cannot read");
    /* one that reaches a game taking none (sent by hand): kept from the game side */
    rc_msg m; memset(&m, 0, sizeof m); m.h.type = RC_MSG_SAY; m.h.sender = 0x500; m.u.say.phrase = 2; m.u.say.seq = 9;
    uint8_t b[RC_MAX_DGRAM]; int n = rc_encode(&m, b, sizeof b);
    rc_receive(&g_n[2], node_ip(0), node_port(0), b, n, g_now);
    CHECK(!g_n[2].n_say_in, "a game that takes no quick chat keeps none");
    g_n[0].conf.say = 0;
    CHECK(rc_send_say(&g_n[0], 7, 2) == 0, "a game that takes no quick chat sends none");
    g_n[0].conf.say = 1;
    /* the inbox holds RC_SAY_INBOX, oldest first */
    for (int k = 0; k < RC_SAY_INBOX + 4; k++) rc_send_say(&g_n[0], (uint8_t)(k & 15), (uint8_t)(10 + k));
    net_run(g_now + 20000, 3);
    int kept = g_n[1].n_say_in, taken = 0; uint32_t seen = 0;
    while (rc_take_say(&g_n[1], &from, &ph, &sq)) if (sq >= 10 && sq < 10 + RC_SAY_INBOX + 4){ taken++; seen |= 1u << (sq - 10); }
    CHECK(kept == RC_SAY_INBOX && g_n[1].say_dropped == 4 && taken == RC_SAY_INBOX && __builtin_popcount(seen) == RC_SAY_INBOX,
          "say inbox: %d kept, %u dropped, %d taken", kept, g_n[1].say_dropped, taken);
}

int main(int argc, char **argv){
    long fuzz = argc > 1 ? atol(argv[1]) : 200000;
    printf("roam_unit: core %d, protocol %d\n", RC_CORE_VER, RC_PROTO_VER);
    t_layout();
    t_roundtrip();
    t_fuzz(fuzz);
    t_session();
    t_review();
    t_hub();
    t_duel();
    t_lobby();
    t_say();
    t_statex();
    t_crews();
    t_snapstates();
    t_bigmesh();
    t_interp();
    t_guard();
    printf("roam_unit: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
