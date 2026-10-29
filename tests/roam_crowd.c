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
 * strcmp, strncmp, strchr, strstr, strlen and memcpy, the compiler may call the others */
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
static uint8_t IO[4096];                     /* a datagram, or the name, car type and parts */
static void io_send(void *u, uint32_t ip, uint16_t port, const uint8_t *b, int n){ (void)u; js_send(ip, port, b, n); }
static void io_event(void *u, int ev, int peer, const char *t){ (void)u; (void)peer; js_event(ev, t, (int)strlen(t)); }

/* the buffer the script writes a name or a received datagram into */
__attribute__((export_name("crowd_io"))) uint8_t *crowd_io(void){ return IO; }

/* name (in IO, 0-terminated), id, room hash, a crew's hash (0: none), the hub's address, our port, the car type's
 * name hash, states per SNAP */
__attribute__((export_name("crowd_init"))) void crowd_init(uint32_t id, uint32_t room, uint32_t crew, uint32_t hub_ip, uint32_t port,
                                                           uint32_t type_hash, uint32_t states, double now_us){
    rc_config cf; memset(&cf, 0, sizeof cf);
    cf.self_id = id; cf.port = (uint16_t)port; cf.room = room; cf.platform = RC_PLAT_TOOL;
    if (crew){ cf.crews[0] = crew; cf.n_crews = 1; }
    cf.max_players = RC_MAX_PLAYERS; cf.rate_hz = 20; cf.snap_states = (int)states; cf.hub_ip = hub_ip; cf.hub_port = (uint16_t)port;
    rc_name_clean(cf.name, (const char *)IO, RC_NAME_MAX);
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
    rc_push_state(&S, &st, (int64_t)now_us);
    rc_tick(&S, (int64_t)now_us);
}
/* in a race (RC_ST_EVENT) or back in free roam: a game in a race sends no
 * car, only its timers run (crowd_wait) */
__attribute__((export_name("crowd_status"))) void crowd_status(int st){ rc_set_status(&S, (uint8_t)st); }
__attribute__((export_name("crowd_wait"))) void crowd_wait(double now_us){ rc_tick(&S, (int64_t)now_us); }
/* players established now */
__attribute__((export_name("crowd_peers"))) int crowd_peers(void){ return rc_established(&S); }
__attribute__((export_name("crowd_stop"))) void crowd_stop(void){ rc_shutdown(&S, 0); }
