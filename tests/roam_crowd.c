/* roam_crowd.c -- one simulated free-roam player for load tests
 * (web/tools/roam-crowd.mjs): the real core (runtime/roam_core.c) in hub
 * mode, built for wasm without a C library and driven from JavaScript.  The
 * script moves it along a track and hands it what arrives; it sends through
 * the import crowd.send, and tells its joins and leaves through crowd.event.
 * Its car is a car of the type the script names, stock or with the parts it
 * names. */
#include "roam_core.h"

__attribute__((import_module("crowd"), import_name("send"))) void js_send(uint32_t ip, uint32_t port, const uint8_t *b, int n);
__attribute__((import_module("crowd"), import_name("event"))) void js_event(int ev, const char *t, int n);

/* the C library this needs: roam_core.c calls memcmp, its name filter (names.h)
 * strcmp, strncmp, strchr, strstr, strlen, strcpy, strcat and memcpy, the compiler may call the others */
void *memcpy(void *d, const void *s, unsigned long n){ unsigned char *o = d; const unsigned char *i = s; while (n--) *o++ = *i++; return d; }
void *memmove(void *d, const void *s, unsigned long n){
    unsigned char *o = d; const unsigned char *i = s;
    if (o < i) while (n--) *o++ = *i++; else { o += n; i += n; while (n--) *--o = *--i; }
    return d;
}
void *memset(void *d, int c, unsigned long n){ unsigned char *o = d; while (n--) *o++ = (unsigned char)c; return d; }
int memcmp(const void *a, const void *b, unsigned long n){
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
}
unsigned long strlen(const char *s){ unsigned long n = 0; while (s[n]) n++; return n; }
char *strcpy(char *d, const char *s){ char *o = d; while ((*o++ = *s++)) {} return d; }
char *strcat(char *d, const char *s){ strcpy(d + strlen(d), s); return d; }
int strcmp(const char *a, const char *b){
    while (*a && *a == *b){ a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, unsigned long n){
    for (; n && *a && *a == *b; n--){ a++; b++; }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}
char *strchr(const char *s, int c){
    for (; ; s++){
        if (*s == (char)c) return (char *)s;
        if (!*s) return 0;
    }
}
char *strstr(const char *h, const char *n){
    for (; ; h++){
        unsigned long i = 0;
        while (n[i] && h[i] == n[i]) i++;
        if (!n[i]) return (char *)h;
        if (!*h) return 0;
    }
}

static rc_session S;
static uint8_t IO[4096];                     /* a datagram, or the name, car type, parts, flag and license plate */
static void io_send(void *u, uint32_t ip, uint16_t port, const uint8_t *b, int n){ (void)u; js_send(ip, port, b, n); }
static void io_event(void *u, int ev, int peer, const char *t){ (void)u; (void)peer; js_event(ev, t, (int)strlen(t)); }

/* the links the players ask the hub for (--links; 0: they say nothing, and get 32), before crowd_init */
static int LINKS;
__attribute__((export_name("crowd_links"))) void crowd_links(uint32_t n){ LINKS = (int)n; }
/* the car's open parts (roam-crowd --open, RC_OPEN_*): sent in every state's extension (the wheels' heights out of the
 * receiver's range: at rest) */
static uint8_t OPEN;
__attribute__((export_name("crowd_open"))) void crowd_open(uint32_t m){ OPEN = (uint8_t)(m & RC_OPEN_ALL); }
/* the car's crash damage (roam-crowd --dents, RC_MSG_DENTS to the games that show it), RC_DMG_MAX dents: mode 1 once;
 * 2 a new set, a new epoch (a repair and a crash), every `ms`; 3 as 2 with bytes at random; before crowd_init */
static int DMG_MODE, DMG_MS = 1000; static uint32_t DMG_SEED = 1, DMG_EPOCH; static int64_t DMG_NEXT;
static uint8_t DMG_D[RC_DMG_MAX * RC_DMG_BYTES];
__attribute__((export_name("crowd_dents"))) void crowd_dents(uint32_t mode, uint32_t ms, uint32_t seed){
    DMG_MODE = (int)mode; DMG_MS = (int)ms; DMG_SEED = seed | 1u; }
static uint32_t dmg_rand(void){ DMG_SEED ^= DMG_SEED << 13; DMG_SEED ^= DMG_SEED >> 17; DMG_SEED ^= DMG_SEED << 5; return DMG_SEED; }
static void dmg_publish(int64_t now){
    DMG_EPOCH++;
    for (int i = 0; i < RC_DMG_MAX; i++){
        uint8_t *b = DMG_D + i * RC_DMG_BYTES;
        if (DMG_MODE == 3){ for (int k = 0; k < RC_DMG_BYTES; k++) b[k] = (uint8_t)dmg_rand(); continue; }
        /* a point on a side of the body's box (x +-2.1 m, y +-0.85 m, z 0.2..1.2 m), pushed in, 5 to 25 cm deep */
        int side = (int)(dmg_rand() % 4u);
        int16_t p[3];
        p[0] = (int16_t)((int)(dmg_rand() % 4200u) - 2100); p[1] = (int16_t)((int)(dmg_rand() % 1700u) - 850);
        p[2] = (int16_t)(200 + (int)(dmg_rand() % 1000u));
        int8_t d[3] = { 0, 0, (int8_t)-20 };
        if (side == 0){ p[0] = 2100; d[0] = -120; } else if (side == 1){ p[0] = -2100; d[0] = 120; }
        else if (side == 2){ p[1] = 850; d[1] = -120; } else { p[1] = -850; d[1] = 120; }
        memcpy(b, p, 6); b[6] = (uint8_t)d[0]; b[7] = (uint8_t)d[1]; b[8] = (uint8_t)d[2];
        b[9] = (uint8_t)(50 + dmg_rand() % 206u); b[10] = (uint8_t)(60 + dmg_rand() % 190u);
    }
    rc_set_dents(&S, (uint8_t)DMG_EPOCH, 0, RC_DMG_MAX, DMG_D, now);
    DMG_NEXT = now + (int64_t)DMG_MS * 1000;
}

/* the buffer the script writes a name or a received datagram into */
__attribute__((export_name("crowd_io"))) uint8_t *crowd_io(void){ return IO; }

/* name, car type, parts, flag and license plate (in IO, each 0-terminated), id, room hash, a crew's hash (0: none), the hub's address, our port, the car type's
 * name hash, states per SNAP */
__attribute__((export_name("crowd_init"))) void crowd_init(uint32_t id, uint32_t room, uint32_t crew, uint32_t hub_ip, uint32_t port,
                                                           uint32_t type_hash, uint32_t states, double now_us){
    rc_config cf; memset(&cf, 0, sizeof cf);
    cf.self_id = id; cf.port = (uint16_t)port; cf.room = room; cf.platform = RC_PLAT_TOOL;
    if (crew){ cf.crews[0] = crew; cf.n_crews = 1; }
    cf.max_players = RC_MAX_PLAYERS; cf.rate_hz = 20; cf.snap_states = (int)states; cf.hub_ip = hub_ip; cf.hub_port = (uint16_t)port;
    cf.links = LINKS;
    rc_name_clean(cf.name, (const char *)IO, RC_NAME_MAX);
    { /* the flag before the name (--flags), after the parts */
      const char *t = (const char *)IO + strlen((const char *)IO) + 1, *pa = t + strlen(t) + 1, *f = pa + strlen(pa) + 1;
      for (int i = 0; i < RC_FLAG_MAX && f[i]; i++) cf.flag[i] = f[i];
      /* the license plate (--plates), after the flag: one rc_plate_ok refuses, none */
      const char *pl = f + strlen(f) + 1;
      size_t n = strlen(pl);
      if (n <= RC_PLATE_MAX && rc_plate_ok(pl, (uint32_t)n)) memcpy(cf.plate, pl, n); }
    rc_io io = { 0, io_send, 0, io_event };
    rc_session_init(&S, &cf, &io);
    rc_carcfg cc; memset(&cc, 0, sizeof cc);
    cc.type_hash = type_hash; cc.ride_b[3] = 1;
    { const char *n = (const char *)IO + strlen((const char *)IO) + 1; for (int i = 0; i < RC_CFG_TYPE_NAME && n[i]; i++) cc.type_name[i] = n[i];
      /* then parts, "slot:key:owner" in hex, comma-separated (--parts) */
      const char *p = n + strlen(n) + 1;
      while (*p && cc.n_parts < RC_PART_SLOTS){
          uint32_t v[3] = { 0, 0, 0 };
          for (int k = 0; k < 3; k++){
              for (; (*p >= '0' && *p <= '9') || ((*p | 0x20) >= 'a' && (*p | 0x20) <= 'f'); p++)
                  v[k] = v[k] << 4 | (uint32_t)(*p <= '9' ? *p - '0' : (*p | 0x20) - 'a' + 10);
              if (*p == ':') p++;
          }
          cc.part[cc.n_parts].slot = (uint8_t)v[0]; cc.part[cc.n_parts].key = v[1]; cc.part[cc.n_parts].owner = v[2]; cc.n_parts++;
          while (*p && *p != ',') p++;
          if (*p == ',') p++;
      } }
    uint8_t rec[RC_CFG_MAX]; int len = rc_carcfg_encode(&cc, rec, sizeof rec);
    if (len > 0) rc_set_cfg(&S, rec, len, (int64_t)now_us);
    rc_set_status(&S, RC_ST_ROAM);
    if (DMG_MODE) dmg_publish((int64_t)now_us);
}
/* a datagram the script put into IO */
__attribute__((export_name("crowd_recv"))) void crowd_recv(uint32_t ip, uint32_t port, int n, double now_us){
    rc_receive(&S, ip, (uint16_t)port, IO, n, (int64_t)now_us);
}
/* where the car is now (heading in radians, 0 = +x), and the session's timers */
__attribute__((export_name("crowd_tick"))) void crowd_tick(double now_us, float x, float y, float z, float vx, float vy, float vz, float heading){
    rc_state st; memset(&st, 0, sizeof st);
    st.t_us = (uint32_t)(int64_t)now_us;
    st.p[0] = x; st.p[1] = y; st.p[2] = z; st.v[0] = vx; st.v[1] = vy; st.v[2] = vz;
    /* the heading as a rotation about z: q = (0, 0, sin h/2, cos h/2), by
     * the series (no libm; |h| <= pi) */
    float a = heading / 2.f, a2 = a * a;
    st.q[2] = a - a * a2 / 6.f + a * a2 * a2 / 120.f - a * a2 * a2 * a2 / 5040.f;
    st.q[3] = 1.f - a2 / 2.f + a2 * a2 / 24.f - a2 * a2 * a2 / 720.f + a2 * a2 * a2 * a2 / 40320.f;
    st.speed = rc_sqrtf(vx * vx + vy * vy); st.rpm = 2500.f; st.gear = 3; st.throttle = 0.5f;
    if (OPEN){ st.ext = 1; st.open = OPEN; for (int w = 0; w < 4; w++) st.wz[w] = 5.f; }
    rc_push_state(&S, &st, (int64_t)now_us);
    if (DMG_MODE >= 2 && (int64_t)now_us >= DMG_NEXT) dmg_publish((int64_t)now_us);
    rc_tick(&S, (int64_t)now_us);
}
/* in a race (RC_ST_EVENT) or back in free roam: a game in a race sends no
 * car, only its timers run (crowd_wait) */
__attribute__((export_name("crowd_status"))) void crowd_status(int st){ rc_set_status(&S, (uint8_t)st); }
__attribute__((export_name("crowd_wait"))) void crowd_wait(double now_us){ rc_tick(&S, (int64_t)now_us); }
/* this player hosts a race lobby (roam-crowd --lobbies): its advert to the
 * hub, as a host's game sends one every second (roam_game.c mp_*); a host
 * takes LOBBY messages, so its HELLOs say so from now on */
__attribute__((export_name("crowd_lobby"))) int crowd_lobby(uint32_t lobby, uint32_t race, int x, int y, int n, int max){
    rc_lobby l; memset(&l, 0, sizeof l);
    S.conf.lobbies = 1;
    l.op = RC_LOBBY_ADVERT; l.state = RC_LS_OPEN; l.max = (uint8_t)max; l.to = 0; l.lobby = lobby; l.race = race;
    l.x = (int16_t)x; l.y = (int16_t)y; l.n = (uint8_t)n; l.ids[0] = S.conf.self_id;
    return rc_send_lobby(&S, &l);
}
/* players established now */
__attribute__((export_name("crowd_peers"))) int crowd_peers(void){ return rc_established(&S); }
__attribute__((export_name("crowd_stop"))) void crowd_stop(void){ rc_shutdown(&S, 0); }
