/* roam_core.c -- free-roam multiplayer: the portable core (see roam_core.h).
 * No OS headers, no libm. */
#include "roam_core.h"
#include "names.h"
/* No <string.h>: the compiler's builtins (inline, or the freestanding
 * memcpy/memset/memmove/memcmp every C platform provides) */
#define memcpy  __builtin_memcpy
#define memset  __builtin_memset
#define memmove __builtin_memmove
#define memcmp  __builtin_memcmp
#define strlen  __builtin_strlen

/* The format assumes these; a platform that breaks them must not build. */
typedef char rc_assert_float[(sizeof(float) == 4) ? 1 : -1];
typedef char rc_assert_u32[(sizeof(uint32_t) == 4) ? 1 : -1];
typedef char rc_assert_i64[(sizeof(int64_t) == 8) ? 1 : -1];
typedef char rc_assert_state[(RC_STATE_BYTES == 52) ? 1 : -1];
typedef char rc_assert_snap[(RC_HDR + 6 + RC_SNAP_STATES * (RC_STATE_BYTES + RC_STATEX_BYTES) + RC_CRC <= RC_MAX_DGRAM) ? 1 : -1];
typedef char rc_assert_cfg[(RC_CFG_MAX <= RC_CFG_FRAG * RC_CFG_MAXFRAG) ? 1 : -1];

/* ---- byte helpers ---------------------------------------------------------- */
void rc_w_init(rc_w *w, uint8_t *buf, uint32_t cap){ w->p = buf; w->n = 0; w->cap = cap; w->err = 0; }
static int w_room(rc_w *w, uint32_t k){ if (w->err || w->cap - w->n < k){ w->err = 1; return 0; } return 1; }
void rc_put8(rc_w *w, uint32_t v){ if (w_room(w, 1)) w->p[w->n++] = (uint8_t)v; }
void rc_put16(rc_w *w, uint32_t v){ if (w_room(w, 2)){ w->p[w->n] = (uint8_t)v; w->p[w->n + 1] = (uint8_t)(v >> 8); w->n += 2; } }
void rc_put32(rc_w *w, uint32_t v){
    if (!w_room(w, 4)) return;
    w->p[w->n] = (uint8_t)v; w->p[w->n + 1] = (uint8_t)(v >> 8);
    w->p[w->n + 2] = (uint8_t)(v >> 16); w->p[w->n + 3] = (uint8_t)(v >> 24); w->n += 4;
}
void rc_putf(rc_w *w, float f){ rc_put32(w, rc_fbits(f)); }
void rc_putb(rc_w *w, const void *src, uint32_t n){ if (w_room(w, n)){ if (n) memcpy(w->p + w->n, src, n); w->n += n; } }
void rc_r_init(rc_r *r, const uint8_t *buf, uint32_t len){ r->p = buf; r->n = 0; r->len = len; r->err = 0; }
static int r_have(rc_r *r, uint32_t k){ if (r->err || r->len - r->n < k){ r->err = 1; return 0; } return 1; }
uint32_t rc_get8(rc_r *r){ return r_have(r, 1) ? r->p[r->n++] : 0u; }
uint32_t rc_get16(rc_r *r){
    if (!r_have(r, 2)) return 0;
    uint32_t v = (uint32_t)r->p[r->n] | (uint32_t)r->p[r->n + 1] << 8; r->n += 2; return v;
}
uint32_t rc_get32(rc_r *r){
    if (!r_have(r, 4)) return 0;
    uint32_t v = (uint32_t)r->p[r->n] | (uint32_t)r->p[r->n + 1] << 8 | (uint32_t)r->p[r->n + 2] << 16 |
                 (uint32_t)r->p[r->n + 3] << 24;
    r->n += 4; return v;
}
float rc_getf(rc_r *r){ return rc_bitsf(rc_get32(r)); }
void rc_getb(rc_r *r, void *dst, uint32_t n){ if (r_have(r, n)){ if (n) memcpy(dst, r->p + r->n, n); r->n += n; } else if (n) memset(dst, 0, n); }

uint32_t rc_fbits(float f){ uint32_t u; memcpy(&u, &f, 4); return u; }
float rc_bitsf(uint32_t u){ float f; memcpy(&f, &u, 4); return f; }
/* one correctly rounded instruction on both targets (sqrtss / fsqrt), never
 * a libm call */
#if defined(__i386__) || defined(__x86_64__)
#include <xmmintrin.h>
float rc_sqrtf(float x){ return x > 0.f ? _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))) : 0.f; }
#else
float rc_sqrtf(float x){ return x > 0.f ? __builtin_sqrtf(x) : 0.f; }
#endif

/* CRC-32 (IEEE 802.3 / zlib polynomial, reflected), bitwise: no table */
uint32_t rc_crc32(const uint8_t *p, uint32_t n){
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++){
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
uint32_t rc_fnv1a(const char *s){
    if (!s || !*s) return 0;
    uint32_t h = 0x811C9DC5u;
    for (; *s; s++){ h ^= (uint8_t)*s; h *= 0x01000193u; }
    return h ? h : 1u;
}
/* the Room setting's hash: rc_fnv1a of the name without its spaces and with its capitals small ("Blue Room" is
 * "blueroom"), so friends who type it a little differently still meet; 0 (no room) for a name of spaces only.  The
 * start page asks the server for a room's players by this hash (web/src/app/players.ts roomHash). */
uint32_t rc_room_hash(const char *s){
    uint32_t h = 0x811C9DC5u; int n = 0;
    for (; s && *s; s++){
        uint8_t b = (uint8_t)*s;
        if (b == ' ' || (b >= '\t' && b <= '\r')) continue;
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        h ^= b; h *= 0x01000193u; n++;
    }
    return !n ? 0 : h ? h : 1u;
}
uint32_t rc_fnv1a_bytes(const uint8_t *p, uint32_t n){
    uint32_t h = 0x811C9DC5u;
    for (uint32_t i = 0; i < n; i++){ h ^= p[i]; h *= 0x01000193u; }
    return h ? h : 1u;
}
void rc_name_clean(char *dst, const char *src, uint32_t srclen){
    uint32_t o = 0;
    for (uint32_t i = 0; src && i < srclen && src[i] && o < RC_NAME_MAX; i++){
        uint8_t ch = (uint8_t)src[i];
        if (ch == ' ' && o == 0) continue;                  /* no leading blanks */
        dst[o++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '?';
    }
    while (o && dst[o - 1] == ' ') o--;
    dst[o] = 0;
    if (!o || name_blocked(dst)) memcpy(dst, "Player", 7);
}
const char *rc_status_name(int st){
    static const char *n[RC_ST__N] = { "menus", "loading", "free roam", "event", "shop", "paused" };
    return (st >= 0 && st < RC_ST__N) ? n[st] : "?";
}

/* ---- float helpers (basic operations only) ----------------------------------- */
static int f_finite(float f){ uint32_t u = rc_fbits(f); return (u & 0x7F800000u) != 0x7F800000u; }
static float f_abs(float f){ return f < 0 ? -f : f; }
static float f_clamp(float f, float lo, float hi){ return f < lo ? lo : f > hi ? hi : f; }
/* round to nearest, saturating, NaN -> 0: the same result on every target
 * (a plain cast of NaN or of a value out of range is undefined: arm64
 * saturates, x86's cvttss2si gives 0x80000000) */
static int32_t f_round(float f){
    if (!(f == f)) return 0;
    if (f >= 2147483520.f) return 2147483647;              /* the largest float below 2^31 */
    if (f <= -2147483520.f) return -2147483647 - 1;
    return (int32_t)(f >= 0 ? f + 0.5f : f - 0.5f);
}
static void q_norm(float *q){
    float n = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
    if (n < 1e-12f){ q[0] = q[1] = q[2] = 0; q[3] = 1; return; }
    float k = 1.f / rc_sqrtf(n);
    for (int i = 0; i < 4; i++) q[i] *= k;
}
float rc_expneg(float x){
    if (!(x > 0)) return 1.f;
    if (x > 30.f) return 0.f;
    /* e^-x = (e^-1)^n * 1/e^f, n = floor(x), f in [0, 1): 8th-order Taylor
     * for e^f (relative error < 3e-6), n exact multiplications */
    int n = (int)x;
    float f = x - (float)n;
    float e = 1.f + f*(1.f + f*(1.f/2.f + f*(1.f/6.f + f*(1.f/24.f + f*(1.f/120.f + f*(1.f/720.f + f*(1.f/5040.f + f*(1.f/40320.f))))))));
    float r = 1.f / e;
    for (int i = 0; i < n; i++) r *= 0.36787944117f;
    return r;
}
float rc_atan2f(float y, float x){
    const float pi = 3.14159265358979f, pi_2 = 1.57079632679490f, pi_6 = 0.52359877559830f;
    float ax = f_abs(x), ay = f_abs(y);
    if (!(ax > 0.f) && !(ay > 0.f)) return 0.f;
    int swap = ay > ax;
    float t = swap ? ax / ay : ay / ax;                     /* 0 .. 1 */
    /* atan t = pi/6 + atan((t*sqrt3 - 1) / (t + sqrt3)) for t > tan(pi/12):
     * the argument is then within +-0.268, where the series to t^13 is
     * below float precision */
    float base = 0.f;
    if (t > 0.26794919243f){ t = (t * 1.73205080757f - 1.f) / (t + 1.73205080757f); base = pi_6; }
    float t2 = t * t;
    float a = t * (1.f + t2*(-1.f/3.f + t2*(1.f/5.f + t2*(-1.f/7.f + t2*(1.f/9.f + t2*(-1.f/11.f + t2*(1.f/13.f)))))));
    a += base;
    if (swap) a = pi_2 - a;
    if (x < 0.f) a = pi - a;
    return y < 0.f ? -a : a;
}
/* rows (forward, left, up) of the game's orientation matrix = the columns of
 * the body->world rotation R */
void rc_m2q(const float *f, const float *l, const float *u, float *q){
    float tr = f[0] + l[1] + u[2];
    if (tr > 0){ float s = rc_sqrtf(tr + 1.f) * 2.f; q[3] = .25f * s; q[0] = (l[2] - u[1]) / s; q[1] = (u[0] - f[2]) / s; q[2] = (f[1] - l[0]) / s; }
    else if (f[0] > l[1] && f[0] > u[2]){ float s = rc_sqrtf(1.f + f[0] - l[1] - u[2]) * 2.f; q[3] = (l[2] - u[1]) / s; q[0] = .25f * s; q[1] = (l[0] + f[1]) / s; q[2] = (u[0] + f[2]) / s; }
    else if (l[1] > u[2]){ float s = rc_sqrtf(1.f + l[1] - f[0] - u[2]) * 2.f; q[3] = (u[0] - f[2]) / s; q[0] = (l[0] + f[1]) / s; q[1] = .25f * s; q[2] = (u[1] + l[2]) / s; }
    else { float s = rc_sqrtf(1.f + u[2] - f[0] - l[1]) * 2.f; q[3] = (f[1] - l[0]) / s; q[0] = (u[0] + f[2]) / s; q[1] = (u[1] + l[2]) / s; q[2] = .25f * s; }
    q_norm(q);
}
void rc_q2m(const float *q, float *f, float *l, float *u){
    float x = q[0], y = q[1], z = q[2], w = q[3];
    f[0] = 1 - 2*(y*y + z*z); f[1] = 2*(x*y + z*w);     f[2] = 2*(x*z - y*w);
    l[0] = 2*(x*y - z*w);     l[1] = 1 - 2*(x*x + z*z); l[2] = 2*(y*z + x*w);
    u[0] = 2*(x*z + y*w);     u[1] = 2*(y*z - x*w);     u[2] = 1 - 2*(x*x + y*y);
}
static void q_mul(const float *a, const float *b, float *o){   /* o = a * b */
    float x = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    float y = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    float z = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    float w = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}
static void q_conj(const float *a, float *o){ o[0] = -a[0]; o[1] = -a[1]; o[2] = -a[2]; o[3] = a[3]; }
static void q_nlerp(const float *a, const float *b, float t, float *o){
    float d = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3], s = d < 0 ? -1.f : 1.f;
    for (int i = 0; i < 4; i++) o[i] = (1.f - t) * a[i] + t * s * b[i];
    q_norm(o);
}
/* q advanced by world angular velocity w over dt seconds (4 first-order steps) */
static void q_integrate(const float *q0, const float *w, float dt, float *o){
    float q[4] = { q0[0], q0[1], q0[2], q0[3] }, h = dt * 0.25f;
    for (int k = 0; k < 4; k++){
        float wq[4] = { w[0], w[1], w[2], 0 }, d[4];
        q_mul(wq, q, d);
        for (int i = 0; i < 4; i++) q[i] += 0.5f * h * d[i];
        q_norm(q);
    }
    for (int i = 0; i < 4; i++) o[i] = q[i];
}

/* ---- states on the wire ------------------------------------------------------ */
static int16_t q16(float f, float scale){ int32_t v = f_round(f * scale); return (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v); }
static void put_state(rc_w *w, const rc_state *s){
    rc_put32(w, s->t_us);
    for (int i = 0; i < 3; i++) rc_putf(w, s->p[i]);
    for (int i = 0; i < 3; i++) rc_putf(w, s->v[i]);
    float q[4] = { s->q[0], s->q[1], s->q[2], s->q[3] }; q_norm(q);
    for (int i = 0; i < 4; i++) rc_put16(w, (uint16_t)q16(q[i], 32767.f));
    for (int i = 0; i < 3; i++) rc_put16(w, (uint16_t)q16(s->w[i], 1000.f));
    rc_put16(w, (uint16_t)q16(f_clamp(s->steer, -1.f, 1.f), 32767.f));
    rc_put8(w, (uint32_t)f_round(f_clamp(s->throttle, 0.f, 1.f) * 255.f));
    rc_put8(w, (uint32_t)f_round(f_clamp(s->brake, 0.f, 1.f) * 255.f));
    rc_put16(w, (uint32_t)f_round(f_clamp(s->rpm, 0.f, 65535.f)));
    rc_put16(w, (uint16_t)q16(f_clamp(s->speed, -327.f, 327.f), 100.f));
    rc_put8(w, (uint8_t)s->gear);
    rc_put8(w, s->bits);
}
/* a wheel's slip on the wire: int8 steps of RC_SLIP_STEP, at most 127 */
static int8_t q_slip(float f){ int32_t v = f_round(f_clamp(f, -1000.f, 1000.f) / RC_SLIP_STEP); return (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v); }
/* the extension (RC_HF_STATEX): a state without one has the length 0; the
 * slip and surfaces only while a wheel slides (all slips 0 on the wire
 * otherwise: the shorter form, as before), the engine only while it sounds */
#define ENG_MAX 20000u
static void put_statex(rc_w *w, const rc_state *s){
    if (!s->ext){ rc_put8(w, 0); return; }
    int slides = 0, eng = s->eng != 0;
    for (int i = 0; i < 8; i++) slides |= q_slip(s->slip[i]) != 0;
    rc_put8(w, (slides ? RC_STATEX_SLIP : RC_STATEX_HYD) + (eng ? RC_STATEX_ENG : 0) - 1);
    for (int i = 0; i < 4; i++) rc_put16(w, (uint16_t)q16(f_clamp(s->wz[i], -32.f, 32.f), 1000.f));
    rc_put8(w, s->nos);
    rc_put8(w, s->hyd); rc_put8(w, s->thumps);
    if (slides) for (int i = 0; i < 4; i++){
        rc_put8(w, (uint8_t)q_slip(s->slip[2 * i])); rc_put8(w, (uint8_t)q_slip(s->slip[2 * i + 1])); rc_put8(w, s->surf[i]);
    }
    if (eng){ rc_put16(w, s->eng < ENG_MAX ? s->eng : ENG_MAX); rc_put8(w, s->eng_acc); rc_put8(w, s->eng_dcl); rc_put8(w, s->eng_base); }
}
static int get_statex(rc_r *r, rc_state *s){
    uint32_t n = rc_get8(r);
    if (r->err) return RC_E_SHORT;
    if (!n) return RC_OK;
    if (n < RC_STATEX_V4 - 1) return RC_E_FIELD;
    if (!r_have(r, n)) return RC_E_SHORT;
    for (int i = 0; i < 4; i++) s->wz[i] = (float)(int16_t)rc_get16(r) / 1000.f;
    s->nos = (uint8_t)rc_get8(r);
    uint32_t k = RC_STATEX_V4 - 1;
    if (n >= RC_STATEX_HYD - 1){ s->hyd = (uint8_t)rc_get8(r); s->thumps = (uint8_t)rc_get8(r); k = RC_STATEX_HYD - 1; }
    if (n >= RC_STATEX_SLIP - 1){
        for (int i = 0; i < 4; i++){
            s->slip[2 * i] = (float)(int8_t)rc_get8(r) * RC_SLIP_STEP; s->slip[2 * i + 1] = (float)(int8_t)rc_get8(r) * RC_SLIP_STEP;
            s->surf[i] = (uint8_t)rc_get8(r);
        }
        s->slipx = 1; k = RC_STATEX_SLIP - 1;
    }
    if (n >= k + RC_STATEX_ENG){                                    /* the engine, after the hydraulics or the slip */
        uint32_t e = rc_get16(r);
        s->eng = (uint16_t)(e < ENG_MAX ? e : ENG_MAX); s->eng_acc = (uint8_t)rc_get8(r); s->eng_dcl = (uint8_t)rc_get8(r);
        s->eng_base = (uint8_t)rc_get8(r);
        k += RC_STATEX_ENG;
    }
    r->n += n - k;                                                  /* a newer core's fields */
    s->ext = 1;
    return RC_OK;
}
static int get_state(rc_r *r, rc_state *s){
    s->ext = 0; s->nos = 0; s->hyd = 0; s->thumps = 0; s->slipx = 0; s->eng = 0; s->eng_acc = 0; s->eng_dcl = 0; s->eng_base = 0;
    for (int i = 0; i < 4; i++){ s->wz[i] = 0; s->surf[i] = 0; }
    for (int i = 0; i < 8; i++) s->slip[i] = 0;
    s->t_us = rc_get32(r);
    for (int i = 0; i < 3; i++) s->p[i] = rc_getf(r);
    for (int i = 0; i < 3; i++) s->v[i] = rc_getf(r);
    for (int i = 0; i < 4; i++) s->q[i] = (float)(int16_t)rc_get16(r) / 32767.f;
    for (int i = 0; i < 3; i++) s->w[i] = (float)(int16_t)rc_get16(r) / 1000.f;
    s->steer = (float)(int16_t)rc_get16(r) / 32767.f;
    s->throttle = (float)rc_get8(r) / 255.f;
    s->brake = (float)rc_get8(r) / 255.f;
    s->rpm = (float)rc_get16(r);
    s->speed = (float)(int16_t)rc_get16(r) / 100.f;
    s->gear = (int8_t)rc_get8(r);
    s->bits = (uint8_t)rc_get8(r);
    if (r->err) return RC_E_SHORT;
    for (int i = 0; i < 3; i++) if (!f_finite(s->p[i]) || !f_finite(s->v[i])) return RC_E_RANGE;
    if (f_abs(s->p[0]) > 20000.f || f_abs(s->p[1]) > 20000.f || f_abs(s->p[2]) > 2000.f) return RC_E_RANGE;
    if (s->v[0]*s->v[0] + s->v[1]*s->v[1] + s->v[2]*s->v[2] > 200.f * 200.f) return RC_E_RANGE;
    float n = s->q[0]*s->q[0] + s->q[1]*s->q[1] + s->q[2]*s->q[2] + s->q[3]*s->q[3];
    if (n < 0.25f) return RC_E_RANGE;
    q_norm(s->q);
    return RC_OK;
}

/* ---- messages ------------------------------------------------------------------- */
static int printable(const char *s, uint32_t n){ for (uint32_t i = 0; i < n; i++) if ((uint8_t)s[i] < 0x20 || (uint8_t)s[i] >= 0x7F) return 0; return 1; }

/* a LOBBY message's fields: a known op, state and reason, an id for the
 * lobby, a race, 2 to RC_LOBBY_MAX players at most and no more in it, no
 * player twice and none 0 */
static int lobby_ok(const rc_lobby *l){
    if (l->op < 1 || l->op >= RC_LOBBY__N || l->state >= RC_LS__N || l->arg >= RC_LL__N || !l->lobby || !l->race) return 0;
    if (l->max < 2 || l->max > RC_LOBBY_MAX || l->n > l->max) return 0;
    for (int i = 0; i < l->n; i++){
        if (!l->ids[i]) return 0;
        for (int j = 0; j < i; j++) if (l->ids[j] == l->ids[i]) return 0;
    }
    return 1;
}
/* a RACEREP's fields: a lobby and a race, 1 to RC_LOBBY_MAX cars and a place among them (0: none), a class, numbers
 * in range, a printable name */
static int in(float f, float lo, float hi){ return f_finite(f) && f >= lo && f <= hi; }
static int racerep_ok(const rc_racerep *r){
    if (!r->lobby || !r->race || r->players < 1 || r->players > RC_LOBBY_MAX || r->place > r->players) return 0;
    if (r->cls < -1 || r->cls > 3 || r->flags & ~(RC_RR_HOST | RC_RR_QUIT)) return 0;
    if (!in(r->rating, 0.f, 5000.f) || !in(r->field, 0.f, 5000.f) || !in(r->factor, 0.01f, 100.f) || !in(r->kw, 0.f, 10000.f)
        || !in(r->kg, 0.f, 50000.f) || !in(r->kmh, 0.f, 2000.f) || !in(r->time, 0.f, 100000.f)) return 0;
    uint32_t n = 0; while (n < RC_RACEREP_NAME && r->name[n]) n++;
    return printable(r->name, n);
}
/* a RACERES's fields: a lobby and a race, 1 to RC_LOBBY_MAX rows, known flags (one row ours at most), numbers in range */
static int raceres_ok(const rc_raceres *r){
    if (!r->lobby || !r->race || r->n < 1 || r->n > RC_LOBBY_MAX) return 0;
    int ours = 0;
    for (int k = 0; k < r->n; k++){
        if (r->row[k].flags & ~(RC_RS_FINISHED | RC_RS_OURS) || !in(r->row[k].time, 0.f, 100000.f) || !in(r->row[k].done, 0.f, 1000.f)
            || !in(r->row[k].dist, 0.f, 1e6f)) return 0;
        ours += (r->row[k].flags & RC_RS_OURS) != 0;
    }
    return ours <= 1;
}
int rc_encode(const rc_msg *m, uint8_t *out, int cap){
    rc_w w; rc_w_init(&w, out, (uint32_t)(cap < RC_MAX_DGRAM ? cap : RC_MAX_DGRAM));
    rc_put8(&w, RC_MAGIC0); rc_put8(&w, RC_MAGIC1); rc_put8(&w, RC_MAGIC2); rc_put8(&w, RC_MAGIC3);
    rc_put8(&w, RC_PROTO_VER); rc_put8(&w, m->h.type); rc_put16(&w, 0);   /* length below */
    rc_put32(&w, m->h.sender); rc_put16(&w, m->h.seq); rc_put16(&w, m->h.flags);
    switch (m->h.type){
    case RC_MSG_HELLO: {
        uint32_t nl = (uint32_t)strlen(m->u.hello.name);
        if (nl > RC_NAME_MAX || m->u.hello.n_seen > RC_MAX_PEERS) return -1;
        rc_put16(&w, m->u.hello.core_ver); rc_put8(&w, m->u.hello.platform); rc_put8(&w, m->u.hello.status);
        rc_put16(&w, m->u.hello.listen_port); rc_put16(&w, m->u.hello.cfg_rev); rc_put32(&w, m->u.hello.room);
        rc_put8(&w, nl); rc_putb(&w, m->u.hello.name, nl);
        rc_put8(&w, m->u.hello.n_seen);
        for (int i = 0; i < m->u.hello.n_seen; i++) rc_put32(&w, m->u.hello.seen[i]);
        if (m->u.hello.n_crews){
            if (!(m->h.flags & RC_HF_CREW) || m->u.hello.n_crews > RC_CREW_MAX) return -1;
            rc_put8(&w, m->u.hello.n_crews);
            for (int i = 0; i < m->u.hello.n_crews; i++){ if (!m->u.hello.crews[i]) return -1; rc_put32(&w, m->u.hello.crews[i]); }
        }
        break; }
    case RC_MSG_PEERS:
        if (m->u.peers.n > RC_MAX_PEERS) return -1;
        rc_put8(&w, m->u.peers.n);
        for (int i = 0; i < m->u.peers.n; i++){
            uint32_t ip = m->u.peers.e[i].ip;
            rc_put8(&w, ip >> 24); rc_put8(&w, ip >> 16); rc_put8(&w, ip >> 8); rc_put8(&w, ip);
            rc_put16(&w, m->u.peers.e[i].port); rc_put32(&w, m->u.peers.e[i].id);
        }
        if (m->h.flags & RC_HF_CREW){                                  /* the hub's crew marks */
            if (m->u.peers.n_mine > RC_CREW_MAX) return -1;
            rc_put8(&w, m->u.peers.n_mine);
            for (int i = 0; i < m->u.peers.n_mine; i++){ if (!m->u.peers.mine[i]) return -1; rc_put32(&w, m->u.peers.mine[i]); }
            for (int i = 0; i < m->u.peers.n; i++) rc_put8(&w, m->u.peers.crew[i]);
        }
        break;
    case RC_MSG_SNAP:
        if (m->u.snap.n < 1 || m->u.snap.n > RC_SNAP_STATES) return -1;
        rc_put8(&w, m->u.snap.status); rc_put8(&w, m->u.snap.epoch); rc_put16(&w, m->u.snap.cfg_rev);
        rc_put8(&w, m->u.snap.n); rc_put8(&w, m->u.snap.clock);
        for (int i = 0; i < m->u.snap.n; i++){
            put_state(&w, &m->u.snap.s[i]);
            if (m->h.flags & RC_HF_STATEX) put_statex(&w, &m->u.snap.s[i]);
        }
        break;
    case RC_MSG_CFG:
        if (m->u.cfg.nfrag < 1 || m->u.cfg.nfrag > RC_CFG_MAXFRAG || m->u.cfg.frag >= m->u.cfg.nfrag ||
            m->u.cfg.total > RC_CFG_MAX || m->u.cfg.dlen > RC_CFG_FRAG) return -1;
        rc_put16(&w, m->u.cfg.cfg_rev); rc_put8(&w, m->u.cfg.frag); rc_put8(&w, m->u.cfg.nfrag);
        rc_put16(&w, m->u.cfg.total); rc_put16(&w, m->u.cfg.dlen); rc_putb(&w, m->u.cfg.data, m->u.cfg.dlen);
        break;
    case RC_MSG_CFG_REQ: rc_put16(&w, m->u.cfgreq.cfg_rev); rc_put8(&w, m->u.cfgreq.have_mask); break;
    case RC_MSG_PING: rc_put32(&w, m->u.ping.t0); break;
    case RC_MSG_PONG: rc_put32(&w, m->u.ping.t0); rc_put32(&w, m->u.ping.t1); rc_put32(&w, m->u.ping.t2); break;
    case RC_MSG_BYE: rc_put8(&w, m->u.bye.reason); break;
    case RC_MSG_DUEL:
        if (m->u.duel.op < 1 || m->u.duel.op >= RC_DUEL__N || !m->u.duel.to || !m->u.duel.duel) return -1;
        rc_put8(&w, m->u.duel.op); rc_put8(&w, m->u.duel.arg); rc_put32(&w, m->u.duel.to); rc_put32(&w, m->u.duel.duel);
        break;
    case RC_MSG_MAPREQ: rc_put16(&w, m->u.mapreq.req); break;
    case RC_MSG_MAP:
        if (m->u.map.nparts < 1 || m->u.map.nparts > RC_MAP_PARTS || m->u.map.part >= m->u.map.nparts || m->u.map.n > RC_MAP_PER) return -1;
        rc_put16(&w, m->u.map.req); rc_put8(&w, m->u.map.part); rc_put8(&w, m->u.map.nparts);
        rc_put16(&w, m->u.map.total); rc_put8(&w, m->u.map.n); rc_putb(&w, m->u.map.data, (uint32_t)m->u.map.n * RC_MAP_ENTRY);
        break;
    case RC_MSG_LOBBY: {
        const rc_lobby *l = &m->u.lobby;
        if (!lobby_ok(l)) return -1;
        rc_put8(&w, l->op); rc_put8(&w, l->arg); rc_put8(&w, l->state); rc_put8(&w, l->max);
        rc_put32(&w, l->to); rc_put32(&w, l->lobby); rc_put32(&w, l->race);
        rc_put16(&w, (uint16_t)l->x); rc_put16(&w, (uint16_t)l->y); rc_put16(&w, l->t_ms); rc_put8(&w, l->n);
        rc_put8(&w, l->away_s);                                           /* (reserved before it: 0) */
        for (int i = 0; i < l->n; i++) rc_put32(&w, l->ids[i]);
        break; }
    case RC_MSG_LOBBIES:
        if (m->u.lobbies.n > RC_LOBBIES_MAX) return -1;
        rc_put16(&w, m->u.lobbies.req); rc_put8(&w, m->u.lobbies.n); rc_putb(&w, m->u.lobbies.data, (uint32_t)m->u.lobbies.n * RC_LOBBIES_ENTRY);
        break;
    case RC_MSG_RACEREP: {
        const rc_racerep *r = &m->u.racerep;
        if (!racerep_ok(r)) return -1;
        rc_put32(&w, r->lobby); rc_put32(&w, r->race); rc_put8(&w, r->mode); rc_put8(&w, r->laps); rc_put16(&w, r->track);
        rc_put8(&w, r->place); rc_put8(&w, r->players); rc_put8(&w, r->flags); rc_put8(&w, r->type); rc_put8(&w, (uint8_t)r->cls);
        rc_put8(&w, 0); rc_put8(&w, 0); rc_put8(&w, 0);
        rc_putf(&w, r->rating); rc_putf(&w, r->field); rc_putf(&w, r->factor); rc_putf(&w, r->kw); rc_putf(&w, r->kg);
        rc_putf(&w, r->kmh); rc_putf(&w, r->time);
        uint8_t nm[RC_RACEREP_NAME] = {0};
        for (int i = 0; i < RC_RACEREP_NAME && r->name[i]; i++) nm[i] = (uint8_t)r->name[i];
        rc_putb(&w, nm, RC_RACEREP_NAME);
        break; }
    case RC_MSG_SAY: rc_put8(&w, m->u.say.phrase); rc_put8(&w, m->u.say.seq); rc_put8(&w, 0); rc_put8(&w, 0); break;
    case RC_MSG_RACERES: {
        const rc_raceres *r = &m->u.raceres;
        if (!raceres_ok(r)) return -1;
        rc_put32(&w, r->lobby); rc_put32(&w, r->race); rc_put8(&w, r->mode); rc_put8(&w, r->laps); rc_put8(&w, r->n); rc_put8(&w, 0);
        for (int k = 0; k < r->n; k++){
            rc_put32(&w, r->row[k].id); rc_put8(&w, r->row[k].flags); rc_put8(&w, 0); rc_put8(&w, 0); rc_put8(&w, 0);
            rc_putf(&w, r->row[k].time); rc_putf(&w, r->row[k].done); rc_putf(&w, r->row[k].dist);
        }
        break; }
    default: return -1;
    }
    if (w.err || w.n + RC_CRC > w.cap) return -1;
    uint32_t len = w.n + RC_CRC;
    out[6] = (uint8_t)len; out[7] = (uint8_t)(len >> 8);
    rc_put32(&w, rc_crc32(out, w.n));
    return w.err ? -1 : (int)w.n;
}

int rc_decode(const uint8_t *buf, int len, rc_msg *m){
    memset(m, 0, sizeof *m);
    if (len < RC_HDR + RC_CRC) return RC_E_SHORT;
    if (len > RC_MAX_DGRAM) return RC_E_LENGTH;
    if (buf[0] != RC_MAGIC0 || buf[1] != RC_MAGIC1 || buf[2] != RC_MAGIC2 || buf[3] != RC_MAGIC3) return RC_E_MAGIC;
    rc_r r; rc_r_init(&r, buf, (uint32_t)len - RC_CRC);
    r.n = 4;
    m->h.ver = (uint8_t)rc_get8(&r);
    if (m->h.ver != RC_PROTO_VER) return RC_E_VERSION;
    m->h.type = (uint8_t)rc_get8(&r); m->h.len = (uint16_t)rc_get16(&r);
    m->h.sender = rc_get32(&r); m->h.seq = (uint16_t)rc_get16(&r); m->h.flags = (uint16_t)rc_get16(&r);
    if (m->h.len != (uint32_t)len) return RC_E_LENGTH;
    uint32_t crc = (uint32_t)buf[len - 4] | (uint32_t)buf[len - 3] << 8 | (uint32_t)buf[len - 2] << 16 | (uint32_t)buf[len - 1] << 24;
    if (crc != rc_crc32(buf, (uint32_t)len - RC_CRC)) return RC_E_CRC;
    if (!m->h.sender) return RC_E_FIELD;
    switch (m->h.type){
    case RC_MSG_HELLO: {
        m->u.hello.core_ver = (uint16_t)rc_get16(&r); m->u.hello.platform = (uint8_t)rc_get8(&r);
        m->u.hello.status = (uint8_t)rc_get8(&r); m->u.hello.listen_port = (uint16_t)rc_get16(&r);
        m->u.hello.cfg_rev = (uint16_t)rc_get16(&r); m->u.hello.room = rc_get32(&r);
        uint32_t nl = rc_get8(&r);
        if (nl > RC_NAME_MAX) return RC_E_FIELD;
        rc_getb(&r, m->u.hello.name, nl); m->u.hello.name[nl] = 0;
        if (r.err) return RC_E_SHORT;
        if (!printable(m->u.hello.name, nl)) return RC_E_FIELD;
        /* every name heard comes through here: one that reads as a slur is shown as none */
        if (name_blocked(m->u.hello.name)) memcpy(m->u.hello.name, "Player", 7);
        m->u.hello.n_seen = (uint8_t)rc_get8(&r);
        if (m->u.hello.n_seen > RC_MAX_PEERS) return RC_E_FIELD;
        for (int i = 0; i < m->u.hello.n_seen; i++) m->u.hello.seen[i] = rc_get32(&r);
        if (m->u.hello.status >= RC_ST__N) return RC_E_FIELD;
        if ((m->h.flags & RC_HF_CREW) && !r.err && r.n < r.len){
            m->u.hello.n_crews = (uint8_t)rc_get8(&r);
            if (m->u.hello.n_crews > RC_CREW_MAX) return RC_E_FIELD;
            for (int i = 0; i < m->u.hello.n_crews; i++) m->u.hello.crews[i] = rc_get32(&r);
            if (r.err) return RC_E_SHORT;
            for (int i = 0; i < m->u.hello.n_crews; i++) if (!m->u.hello.crews[i]) return RC_E_FIELD;
            r.n = r.len;                                              /* (a newer core's additions) */
        }
        break; }
    case RC_MSG_PEERS:
        m->u.peers.n = (uint8_t)rc_get8(&r);
        if (m->u.peers.n > RC_MAX_PEERS) return RC_E_FIELD;
        for (int i = 0; i < m->u.peers.n; i++){
            uint32_t a = rc_get8(&r), b = rc_get8(&r), c = rc_get8(&r), d = rc_get8(&r);
            m->u.peers.e[i].ip = a << 24 | b << 16 | c << 8 | d;
            m->u.peers.e[i].port = (uint16_t)rc_get16(&r); m->u.peers.e[i].id = rc_get32(&r);
            if (!r.err && (!m->u.peers.e[i].port || !m->u.peers.e[i].id)) return RC_E_FIELD;
        }
        if ((m->h.flags & RC_HF_CREW) && !r.err && r.n < r.len){       /* the hub's crew marks */
            m->u.peers.n_mine = (uint8_t)rc_get8(&r);
            if (!r.err && m->u.peers.n_mine > RC_CREW_MAX) return RC_E_FIELD;
            for (int i = 0; i < m->u.peers.n_mine; i++) m->u.peers.mine[i] = rc_get32(&r);
            for (int i = 0; i < m->u.peers.n; i++) m->u.peers.crew[i] = (uint8_t)rc_get8(&r);
            if (r.err) return RC_E_SHORT;
            for (int i = 0; i < m->u.peers.n_mine; i++) if (!m->u.peers.mine[i]) return RC_E_FIELD;
            r.n = r.len;                                              /* (a newer hub's additions) */
        }
        break;
    case RC_MSG_SNAP: {
        m->u.snap.status = (uint8_t)rc_get8(&r); m->u.snap.epoch = (uint8_t)rc_get8(&r);
        m->u.snap.cfg_rev = (uint16_t)rc_get16(&r); m->u.snap.n = (uint8_t)rc_get8(&r); m->u.snap.clock = (uint8_t)rc_get8(&r);
        if (r.err) return RC_E_SHORT;
        if (m->u.snap.n < 1 || m->u.snap.n > RC_SNAP_STATES || m->u.snap.status >= RC_ST__N) return RC_E_FIELD;
        for (int i = 0; i < m->u.snap.n; i++){
            int e = get_state(&r, &m->u.snap.s[i]); if (e) return e;
            if ((m->h.flags & RC_HF_STATEX) && (e = get_statex(&r, &m->u.snap.s[i]))) return e;
        }
        break; }
    case RC_MSG_CFG:
        m->u.cfg.cfg_rev = (uint16_t)rc_get16(&r); m->u.cfg.frag = (uint8_t)rc_get8(&r);
        m->u.cfg.nfrag = (uint8_t)rc_get8(&r); m->u.cfg.total = (uint16_t)rc_get16(&r);
        m->u.cfg.dlen = (uint16_t)rc_get16(&r);
        if (r.err) return RC_E_SHORT;
        if (m->u.cfg.nfrag < 1 || m->u.cfg.nfrag > RC_CFG_MAXFRAG || m->u.cfg.frag >= m->u.cfg.nfrag ||
            m->u.cfg.total > RC_CFG_MAX || m->u.cfg.dlen > RC_CFG_FRAG) return RC_E_FIELD;
        /* nfrag = ceil(total / RC_CFG_FRAG); fragments are RC_CFG_FRAG bytes
         * except the last, which holds the rest */
        { uint32_t total = m->u.cfg.total, nf = (total + RC_CFG_FRAG - 1u) / RC_CFG_FRAG;
          uint32_t off = (uint32_t)m->u.cfg.frag * RC_CFG_FRAG;
          if (!total || nf != m->u.cfg.nfrag) return RC_E_FIELD;
          uint32_t want = m->u.cfg.frag + 1u < nf ? RC_CFG_FRAG : total - off;
          if (m->u.cfg.dlen != want) return RC_E_FIELD; }
        if (!r_have(&r, m->u.cfg.dlen)) return RC_E_SHORT;
        m->u.cfg.data = r.p + r.n; r.n += m->u.cfg.dlen;
        break;
    case RC_MSG_CFG_REQ: m->u.cfgreq.cfg_rev = (uint16_t)rc_get16(&r); m->u.cfgreq.have_mask = (uint8_t)rc_get8(&r); break;
    case RC_MSG_PING: m->u.ping.t0 = rc_get32(&r); break;
    case RC_MSG_PONG: m->u.ping.t0 = rc_get32(&r); m->u.ping.t1 = rc_get32(&r); m->u.ping.t2 = rc_get32(&r); break;
    case RC_MSG_BYE: m->u.bye.reason = (uint8_t)rc_get8(&r); break;
    case RC_MSG_DUEL:
        m->u.duel.op = (uint8_t)rc_get8(&r); m->u.duel.arg = (uint8_t)rc_get8(&r);
        m->u.duel.to = rc_get32(&r); m->u.duel.duel = rc_get32(&r);
        if (r.err) return RC_E_SHORT;
        if (m->u.duel.op < 1 || m->u.duel.op >= RC_DUEL__N || !m->u.duel.to || !m->u.duel.duel) return RC_E_FIELD;
        break;
    case RC_MSG_MAPREQ: m->u.mapreq.req = (uint16_t)rc_get16(&r); break;
    case RC_MSG_MAP:
        m->u.map.req = (uint16_t)rc_get16(&r); m->u.map.part = (uint8_t)rc_get8(&r); m->u.map.nparts = (uint8_t)rc_get8(&r);
        m->u.map.total = (uint16_t)rc_get16(&r); m->u.map.n = (uint8_t)rc_get8(&r);
        if (r.err) return RC_E_SHORT;
        if (m->u.map.nparts < 1 || m->u.map.nparts > RC_MAP_PARTS || m->u.map.part >= m->u.map.nparts || m->u.map.n > RC_MAP_PER) return RC_E_FIELD;
        if (!r_have(&r, (uint32_t)m->u.map.n * RC_MAP_ENTRY)) return RC_E_SHORT;
        m->u.map.data = r.p + r.n; r.n += (uint32_t)m->u.map.n * RC_MAP_ENTRY;
        break;
    case RC_MSG_LOBBY: {
        rc_lobby *l = &m->u.lobby;
        l->op = (uint8_t)rc_get8(&r); l->arg = (uint8_t)rc_get8(&r); l->state = (uint8_t)rc_get8(&r); l->max = (uint8_t)rc_get8(&r);
        l->to = rc_get32(&r); l->lobby = rc_get32(&r); l->race = rc_get32(&r);
        l->x = (int16_t)rc_get16(&r); l->y = (int16_t)rc_get16(&r); l->t_ms = (uint16_t)rc_get16(&r); l->n = (uint8_t)rc_get8(&r);
        l->away_s = (uint8_t)rc_get8(&r);
        if (r.err) return RC_E_SHORT;
        if (l->n > RC_LOBBY_MAX) return RC_E_FIELD;
        for (int i = 0; i < l->n; i++) l->ids[i] = rc_get32(&r);
        if (r.err) return RC_E_SHORT;
        if (!lobby_ok(l)) return RC_E_FIELD;
        break; }
    case RC_MSG_LOBBIES:
        m->u.lobbies.req = (uint16_t)rc_get16(&r); m->u.lobbies.n = (uint8_t)rc_get8(&r);
        if (r.err) return RC_E_SHORT;
        if (m->u.lobbies.n > RC_LOBBIES_MAX) return RC_E_FIELD;
        if (!r_have(&r, (uint32_t)m->u.lobbies.n * RC_LOBBIES_ENTRY)) return RC_E_SHORT;
        m->u.lobbies.data = r.p + r.n; r.n += (uint32_t)m->u.lobbies.n * RC_LOBBIES_ENTRY;
        break;
    case RC_MSG_RACEREP: {
        rc_racerep *q = &m->u.racerep;
        q->lobby = rc_get32(&r); q->race = rc_get32(&r); q->mode = (uint8_t)rc_get8(&r); q->laps = (uint8_t)rc_get8(&r);
        q->track = (uint16_t)rc_get16(&r); q->place = (uint8_t)rc_get8(&r); q->players = (uint8_t)rc_get8(&r);
        q->flags = (uint8_t)rc_get8(&r); q->type = (uint8_t)rc_get8(&r); q->cls = (int8_t)rc_get8(&r);
        (void)rc_get8(&r); (void)rc_get8(&r); (void)rc_get8(&r);
        q->rating = rc_getf(&r); q->field = rc_getf(&r); q->factor = rc_getf(&r); q->kw = rc_getf(&r); q->kg = rc_getf(&r);
        q->kmh = rc_getf(&r); q->time = rc_getf(&r);
        rc_getb(&r, q->name, RC_RACEREP_NAME); q->name[RC_RACEREP_NAME] = 0;
        if (r.err) return RC_E_SHORT;
        if (!racerep_ok(q)) return RC_E_FIELD;
        break; }
    case RC_MSG_SAY:
        /* the phrase as it came: a newer game may have more of them (the game side ignores one it does not know) */
        m->u.say.phrase = (uint8_t)rc_get8(&r); m->u.say.seq = (uint8_t)rc_get8(&r); (void)rc_get8(&r); (void)rc_get8(&r);
        break;
    case RC_MSG_RACERES: {
        rc_raceres *q = &m->u.raceres;
        q->lobby = rc_get32(&r); q->race = rc_get32(&r); q->mode = (uint8_t)rc_get8(&r); q->laps = (uint8_t)rc_get8(&r);
        q->n = (uint8_t)rc_get8(&r); (void)rc_get8(&r);
        if (r.err) return RC_E_SHORT;
        if (q->n < 1 || q->n > RC_LOBBY_MAX) return RC_E_FIELD;
        for (int k = 0; k < q->n; k++){
            q->row[k].id = rc_get32(&r); q->row[k].flags = (uint8_t)rc_get8(&r); (void)rc_get8(&r); (void)rc_get8(&r); (void)rc_get8(&r);
            q->row[k].time = rc_getf(&r); q->row[k].done = rc_getf(&r); q->row[k].dist = rc_getf(&r);
        }
        if (r.err) return RC_E_SHORT;
        if (!raceres_ok(q)) return RC_E_FIELD;
        break; }
    default: return RC_E_TYPE;
    }
    if (r.err) return RC_E_SHORT;
    if (r.n != r.len) return RC_E_TRAIL;
    return RC_OK;
}

/* ---- car-look record --------------------------------------------------------------- */
#define RC_CFGREC_VER 1
int rc_carcfg_encode(const rc_carcfg *c, uint8_t *out, int cap){
    rc_w w; rc_w_init(&w, out, (uint32_t)(cap < RC_CFG_MAX ? cap : RC_CFG_MAX));
    if (c->n_parts > RC_PART_SLOTS) return -1;
    char tn[RC_CFG_TYPE_NAME]; memset(tn, 0, sizeof tn);
    for (int i = 0; i < RC_CFG_TYPE_NAME && c->type_name[i]; i++) tn[i] = c->type_name[i];
    rc_put8(&w, RC_CFGREC_VER); rc_put32(&w, c->type_hash); rc_putb(&w, tn, RC_CFG_TYPE_NAME);
    rc_putb(&w, c->ride_b, 4);
    rc_put8(&w, c->n_parts);
    for (int i = 0; i < c->n_parts; i++){ rc_put8(&w, c->part[i].slot); rc_put32(&w, c->part[i].key); rc_put32(&w, c->part[i].owner); }
    /* extensions: count, then {u8 tag, u8 length, bytes}; unknown tags are
     * skipped by the reader */
    rc_put8(&w, (c->has_perf ? 1u : 0u) + (c->has_horn ? 1u : 0u));
    if (c->has_perf){ rc_put8(&w, RC_EXT_PERF); rc_put8(&w, RC_PERF_BYTES); rc_putb(&w, c->perf, RC_PERF_BYTES); }
    if (c->has_horn){ rc_put8(&w, RC_EXT_HORN); rc_put8(&w, 1); rc_put8(&w, c->horn); }
    return w.err ? -1 : (int)w.n;
}
int rc_carcfg_decode(const uint8_t *p, int len, rc_carcfg *c){
    memset(c, 0, sizeof *c);
    if (len < 1 || len > RC_CFG_MAX) return RC_E_LENGTH;
    rc_r r; rc_r_init(&r, p, (uint32_t)len);
    if (rc_get8(&r) != RC_CFGREC_VER) return RC_E_VERSION;
    c->type_hash = rc_get32(&r);
    rc_getb(&r, c->type_name, RC_CFG_TYPE_NAME); c->type_name[RC_CFG_TYPE_NAME] = 0;
    { int k = 0; while (k < RC_CFG_TYPE_NAME && c->type_name[k]) k++;
      if (!printable(c->type_name, (uint32_t)k)) return RC_E_FIELD;
      for (int i = k; i < RC_CFG_TYPE_NAME; i++) if (c->type_name[i]) return RC_E_FIELD; }
    rc_getb(&r, c->ride_b, 4);
    uint32_t n = rc_get8(&r);
    if (r.err) return RC_E_SHORT;
    if (n > RC_PART_SLOTS) return RC_E_FIELD;
    uint8_t seen[RC_PART_SLOTS]; memset(seen, 0, sizeof seen);
    for (uint32_t i = 0; i < n; i++){
        uint32_t s = rc_get8(&r);
        c->part[i].slot = (uint8_t)s; c->part[i].key = rc_get32(&r); c->part[i].owner = rc_get32(&r);
        if (r.err) return RC_E_SHORT;
        if (s >= RC_PART_SLOTS || seen[s]) return RC_E_FIELD;
        seen[s] = 1;
    }
    c->n_parts = (uint8_t)n;
    uint32_t next = rc_get8(&r);
    for (uint32_t i = 0; i < next && !r.err; i++){
        uint32_t tag = rc_get8(&r), l = rc_get8(&r);
        if (tag == RC_EXT_PERF && l == RC_PERF_BYTES){ rc_getb(&r, c->perf, RC_PERF_BYTES); c->has_perf = !r.err; }
        else if (tag == RC_EXT_HORN && l == 1){ c->horn = (uint8_t)rc_get8(&r); c->has_horn = !r.err; }
        else if (r_have(&r, l)) r.n += l;
    }
    if (r.err) return RC_E_SHORT;
    if (r.n != r.len) return RC_E_TRAIL;
    return RC_OK;
}

/* ---- interpolation ------------------------------------------------------------------ */
void rc_interp_reset(rc_interp *ip){ memset(ip, 0, sizeof *ip); ip->err_q[3] = 1; ip->prev_q[3] = 1; }

static int64_t unwrap(rc_interp *ip, uint32_t t32){
    if (!ip->t_init){ ip->t_init = 1; ip->t_last = (int64_t)t32 + ((int64_t)1 << 32); return ip->t_last; }
    int64_t t = ip->t_last + (int32_t)(t32 - (uint32_t)ip->t_last);
    if (t > ip->t_last) ip->t_last = t;
    return t;
}
void rc_interp_insert(rc_interp *ip, const rc_state *s, uint8_t epoch, uint8_t clock, uint8_t status, int64_t recv_us, int newest){
    /* epochs only move forward (mod 256): a state of an older epoch, or of an
     * older clock epoch, is a datagram sent before the teleport or the
     * re-anchor that arrived late (reordered on the way); taken as new it
     * would throw the car back for one snapshot interval */
    if (ip->epoch_init && (int8_t)(uint8_t)(epoch - ip->epoch) < 0){ ip->n_stale++; return; }
    if (ip->epoch_init && epoch == ip->epoch && ip->clock_init && (int8_t)(uint8_t)(clock - ip->clock) < 0){ ip->n_stale++; return; }
    if (ip->epoch_init && epoch == ip->epoch && ip->clock_init && clock != ip->clock){
        /* the sender re-anchored its clock (after a pause or a long stall):
         * the old states and offsets belong to the old time line; what is
         * shown stays, so the change is blended, not popped */
        ip->n = 0; ip->t_init = 0; ip->odn = ip->odh = 0; ip->off_init = 0;
        ip->clock = clock; ip->n_epoch++;
    }
    if (!ip->epoch_init || epoch != ip->epoch){
        /* new epoch (teleport, clock re-anchor, restart): forget the past */
        int had = ip->epoch_init;
        uint64_t keep[9] = { ip->n_eval, ip->n_interp, ip->n_extrap, ip->n_hold, ip->n_dup, ip->n_old, ip->n_snap, ip->n_epoch, ip->n_stale };
        float em = ip->err_max;
        rc_interp_reset(ip);
        ip->n_eval = keep[0]; ip->n_interp = keep[1]; ip->n_extrap = keep[2]; ip->n_hold = keep[3];
        ip->n_dup = keep[4]; ip->n_old = keep[5]; ip->n_snap = keep[6]; ip->n_epoch = keep[7] + (had ? 1 : 0); ip->n_stale = keep[8];
        ip->err_max = em;
        ip->epoch_init = 1; ip->epoch = epoch; ip->snap_pending = 1;
        ip->clock_init = 1; ip->clock = clock;
    }
    ip->status = status;
    int64_t t = unwrap(ip, s->t_us);
    if (newest){
        int k = ip->odh; ip->od[k].at = recv_us; ip->od[k].d = recv_us - t;
        ip->odh = (ip->odh + 1) % RC_OFFWIN; if (ip->odn < RC_OFFWIN) ip->odn++;
    }
    int i = ip->n;
    while (i > 0 && ip->t[i - 1] > t) i--;
    if (i > 0 && ip->t[i - 1] == t){ ip->n_dup++; return; }
    if (ip->n == RC_BUF_STATES){
        if (i == 0){ ip->n_old++; return; }                       /* older than everything kept */
        memmove(&ip->t[0], &ip->t[1], sizeof ip->t[0] * (size_t)(RC_BUF_STATES - 1));
        memmove(&ip->s[0], &ip->s[1], sizeof ip->s[0] * (size_t)(RC_BUF_STATES - 1));
        ip->n--; i--;
    }
    memmove(&ip->t[i + 1], &ip->t[i], sizeof ip->t[0] * (size_t)(ip->n - i));
    memmove(&ip->s[i + 1], &ip->s[i], sizeof ip->s[0] * (size_t)(ip->n - i));
    ip->t[i] = t; ip->s[i] = *s; ip->n++;
}
/* receiver host time minus sender time: the smallest over the last 2 s */
static int off_estimate(const rc_interp *ip, int64_t *est){
    int64_t newest = 0; int have = 0;
    for (int k = 0; k < ip->odn; k++) if (!have || ip->od[k].at > newest){ newest = ip->od[k].at; have = 1; }
    if (!have) return 0;
    int64_t m = 0; int got = 0;
    for (int k = 0; k < ip->odn; k++)
        if (ip->od[k].at >= newest - 2000000 && (!got || ip->od[k].d < m)){ m = ip->od[k].d; got = 1; }
    *est = m; return got;
}
static void lerp_rest(const rc_state *a, const rc_state *b, float u, rc_pose *o){
    for (int i = 0; i < 3; i++){ o->v[i] = a->v[i] + u * (b->v[i] - a->v[i]); o->w[i] = a->w[i] + u * (b->w[i] - a->w[i]); }
    o->steer = a->steer + u * (b->steer - a->steer);
    o->throttle = a->throttle + u * (b->throttle - a->throttle);
    o->brake = a->brake + u * (b->brake - a->brake);
    o->rpm = a->rpm + u * (b->rpm - a->rpm);
    o->speed = a->speed + u * (b->speed - a->speed);
    const rc_state *n = u < 0.5f ? a : b;
    o->gear = n->gear; o->bits = n->bits;
    o->ext = n->ext; o->nos = n->nos; o->hyd = n->hyd; o->thumps = n->thumps;
    for (int i = 0; i < 4; i++) o->wz[i] = a->ext && b->ext ? a->wz[i] + u * (b->wz[i] - a->wz[i]) : n->wz[i];
    o->slipx = n->slipx;
    for (int i = 0; i < 4; i++) o->surf[i] = n->surf[i];
    for (int i = 0; i < 8; i++) o->slip[i] = a->slipx && b->slipx ? a->slip[i] + u * (b->slip[i] - a->slip[i]) : n->slip[i];
    if (a->eng && b->eng){
        o->eng = (float)a->eng + u * (float)(b->eng - a->eng);
        o->eng_acc = ((float)a->eng_acc + u * (float)(b->eng_acc - a->eng_acc)) / 255.f;
        o->eng_dcl = ((float)a->eng_dcl + u * (float)(b->eng_dcl - a->eng_dcl)) / 255.f;
        o->eng_base = ((float)a->eng_base + u * (float)(b->eng_base - a->eng_base)) / 255.f;
    } else { o->eng = n->eng; o->eng_acc = n->eng_acc / 255.f; o->eng_dcl = n->eng_dcl / 255.f; o->eng_base = n->eng_base / 255.f; }
}
static void from_state(const rc_state *s, rc_pose *o){
    for (int i = 0; i < 3; i++){ o->p[i] = s->p[i]; o->v[i] = s->v[i]; o->w[i] = s->w[i]; }
    for (int i = 0; i < 4; i++){ o->q[i] = s->q[i]; o->wz[i] = s->wz[i]; }
    o->steer = s->steer; o->throttle = s->throttle; o->brake = s->brake; o->rpm = s->rpm; o->speed = s->speed;
    o->gear = s->gear; o->bits = s->bits; o->ext = s->ext; o->nos = s->nos; o->hyd = s->hyd; o->thumps = s->thumps;
    o->slipx = s->slipx;
    for (int i = 0; i < 4; i++) o->surf[i] = s->surf[i];
    for (int i = 0; i < 8; i++) o->slip[i] = s->slip[i];
    o->eng = s->eng; o->eng_acc = s->eng_acc / 255.f; o->eng_dcl = s->eng_dcl / 255.f; o->eng_base = s->eng_base / 255.f;
}
int rc_interp_peek(const rc_interp *ip, int64_t rt, int64_t cap_us, rc_pose *o){
    memset(o, 0, sizeof *o); o->q[3] = 1;
    if (ip->n == 0) return 0;
    if (rt <= ip->t[0]){ from_state(&ip->s[0], o); o->mode = RC_MODE_HOLD; return 1; }
    int n = ip->n;
    if (rt >= ip->t[n - 1]){
        const rc_state *s = &ip->s[n - 1];
        from_state(s, o);
        int64_t dt = rt - ip->t[n - 1];
        if (ip->status == RC_ST_PAUSED){ o->mode = RC_MODE_HOLD; for (int i = 0; i < 3; i++){ o->v[i] = 0; o->w[i] = 0; } o->speed = 0; return 1; }
        o->mode = dt > cap_us ? RC_MODE_HOLD : RC_MODE_EXTRAP;
        if (dt > cap_us) dt = cap_us;
        float ds = (float)(int32_t)dt * 1e-6f;                        /* dt <= cap_us */
        for (int i = 0; i < 3; i++) o->p[i] = s->p[i] + s->v[i] * ds;
        q_integrate(s->q, s->w, ds, o->q);
        return 1;
    }
    int k = 0;
    while (k + 2 < n && ip->t[k + 1] <= rt) k++;
    const rc_state *a = &ip->s[k], *b = &ip->s[k + 1];
    int64_t span = ip->t[k + 1] - ip->t[k], into = rt - ip->t[k];
    if (span > 600000000) span = 600000000;                           /* keep it in int32 */
    if (into > span) into = span;
    if (into < 0) into = 0;
    float dt = (float)(int32_t)span * 1e-6f, u = (float)(int32_t)into / (float)(int32_t)span;
    u = f_clamp(u, 0.f, 1.f);
    /* cubic Hermite with the sender's velocities as tangents, unless the two
     * states disagree with each other (a jump the epoch did not announce):
     * then linear */
    float gap = 0;
    for (int i = 0; i < 3; i++){ float e = b->p[i] - (a->p[i] + 0.5f * (a->v[i] + b->v[i]) * dt); gap += e * e; }
    if (gap > 25.f || dt > 1.f){
        for (int i = 0; i < 3; i++) o->p[i] = a->p[i] + u * (b->p[i] - a->p[i]);
    } else {
        float u2 = u * u, u3 = u2 * u;
        float h00 = 2*u3 - 3*u2 + 1, h10 = u3 - 2*u2 + u, h01 = -2*u3 + 3*u2, h11 = u3 - u2;
        for (int i = 0; i < 3; i++) o->p[i] = h00 * a->p[i] + h10 * dt * a->v[i] + h01 * b->p[i] + h11 * dt * b->v[i];
    }
    q_nlerp(a->q, b->q, u, o->q);
    lerp_rest(a, b, u, o);
    o->mode = RC_MODE_INTERP;
    return 1;
}
int rc_interp_eval(rc_interp *ip, int64_t now_us, int64_t frame_dt_us, int64_t delay_us, int64_t cap_us, rc_pose *out){
    if (ip->n == 0){ memset(out, 0, sizeof *out); out->q[3] = 1; return 0; }
    int64_t est;
    if (off_estimate(ip, &est)){
        if (!ip->off_init || ip->snap_pending){ ip->off_r = est; ip->off_init = 1; }
        else {
            int32_t fdt = frame_dt_us > 1000000 ? 1000000 : frame_dt_us < 0 ? 0 : (int32_t)frame_dt_us;
            int64_t d = est - ip->off_r, lim = fdt / 20 + 1;              /* slew at most 5 % */
            if (d > 250000 || d < -250000){ ip->off_r = est; ip->n_snap++; }
            else ip->off_r += d > lim ? lim : d < -lim ? -lim : d;
        }
    }
    if (!ip->off_init){ memset(out, 0, sizeof *out); out->q[3] = 1; return 0; }
    int64_t rt = now_us - ip->off_r - delay_us;
    rc_pose tgt; rc_interp_peek(ip, rt, cap_us, &tgt);
    ip->n_eval++;
    if (tgt.mode == RC_MODE_INTERP) ip->n_interp++; else if (tgt.mode == RC_MODE_EXTRAP) ip->n_extrap++; else ip->n_hold++;
    if (!ip->shown || ip->snap_pending){
        for (int i = 0; i < 3; i++) ip->err_p[i] = 0;
        ip->err_q[0] = ip->err_q[1] = ip->err_q[2] = 0; ip->err_q[3] = 1;
        ip->snap_pending = 0;
    } else {
        /* what the current buffer says about last frame's render time, against
         * what was shown then: new data since then is a correction, which is
         * shown as an offset that decays (no pop) */
        rc_pose was; rc_interp_peek(ip, ip->prev_rt, cap_us, &was);
        float e2 = 0;
        for (int i = 0; i < 3; i++){ ip->err_p[i] += ip->prev_p[i] - was.p[i]; e2 += ip->err_p[i] * ip->err_p[i]; }
        float cq[4], ic[4], t4[4];
        q_conj(was.q, ic); q_mul(ip->prev_q, ic, cq);               /* shown * was^-1 */
        q_mul(cq, ip->err_q, t4); q_norm(t4);
        for (int i = 0; i < 4; i++) ip->err_q[i] = t4[i];
        int32_t fdt = frame_dt_us > 1000000 ? 1000000 : frame_dt_us < 0 ? 0 : (int32_t)frame_dt_us;
        float k = rc_expneg((float)fdt / 100000.f);                 /* tau = 100 ms */
        float em = rc_sqrtf(e2);
        if (em > ip->err_max) ip->err_max = em;
        if (em > 5.f){ for (int i = 0; i < 3; i++) ip->err_p[i] = 0; ip->err_q[0] = ip->err_q[1] = ip->err_q[2] = 0; ip->err_q[3] = 1; ip->n_snap++; }
        else {
            for (int i = 0; i < 3; i++) ip->err_p[i] *= k;
            static const float id[4] = { 0, 0, 0, 1 };
            float r[4]; q_nlerp(id, ip->err_q, k, r);
            for (int i = 0; i < 4; i++) ip->err_q[i] = r[i];
        }
    }
    ip->shown = 1; ip->prev_rt = rt;
    for (int i = 0; i < 3; i++) ip->prev_p[i] = tgt.p[i];
    for (int i = 0; i < 4; i++) ip->prev_q[i] = tgt.q[i];
    *out = tgt;
    for (int i = 0; i < 3; i++) out->p[i] = tgt.p[i] + ip->err_p[i];
    q_mul(ip->err_q, tgt.q, out->q); q_norm(out->q);
    return 1;
}

/* ---- session --------------------------------------------------------------------------- */
#define T_HELLO_CAND   1000000      /* until established */
#define T_HELLO_EST    2000000
#define T_PING         1000000
#define T_PEERS       10000000
#define T_BEACON       2000000
#define T_LOST         3000000      /* hide the car */
#define T_DROP        10000000      /* forget the peer */
#define T_GOSSIP_FORGET 60000000
#define T_CFGREQ        500000
#define T_CFG_ANSWER    250000      /* the record to one peer, asked for or at its join: at most this often */
#define T_MISMATCH_NOTE 30000000    /* a protocol or room mismatch is noted at most this often */
#define T_HUB_HELLO    1000000      /* to the hub: a HELLO this often */
#define T_HUB_SNAP      500000      /* and a SNAP (where we are) this often */
#define T_HUB_GRACE    2500000      /* a peer the hub has not listed for this long goes (its list comes every second, twice since 2026-10-23) */
#define T_HUB_SILENT  10000000      /* no list for this long (or from the start): discovery and gossip again */
#define RL_DGRAMS      100          /* per second and source */
#define RL_BYTES     65536

/* text for notes, without the C library */
static void cat(char *t, uint32_t *n, uint32_t cap, const char *x){ for (; x && *x && *n + 1 < cap; x++) t[(*n)++] = *x; t[*n] = 0; }
static void cat_u(char *t, uint32_t *n, uint32_t cap, uint32_t v){
    char d[10]; uint32_t k = 0;
    do { d[k++] = (char)('0' + v % 10u); v /= 10u; } while (v && k < 10);
    while (k && *n + 1 < cap) t[(*n)++] = d[--k];
    t[*n] = 0;
}
static void cat_addr(char *t, uint32_t *n, uint32_t cap, uint32_t ip, uint16_t port){
    for (int k = 3; k >= 0; k--){ cat_u(t, n, cap, (ip >> (8 * k)) & 255u); cat(t, n, cap, k ? "." : ":"); }
    cat_u(t, n, cap, port);
}
static void note(rc_session *s, int ev, int peer, const char *a, const char *b, const char *c){
    if (!s->io.event) return;
    char t[160]; uint32_t n = 0;
    const char *parts[3] = { a, b, c };
    for (int k = 0; k < 3; k++) for (const char *p = parts[k]; p && *p && n + 1 < sizeof t; p++) t[n++] = *p;
    t[n] = 0;
    s->io.event(s->io.user, ev, peer, t);
}
/* the header's sequence number counts snapshots (rc_push_state): the same
 * SNAP goes to every peer, the other messages go to one peer each and carry
 * the current value, so a receiver's gap count is the snapshots it missed */
static void hdr(rc_session *s, rc_msg *m, int type){ memset(m, 0, sizeof *m); m->h.type = (uint8_t)type; m->h.sender = s->conf.self_id; m->h.seq = s->seq; }
static void tx_to(rc_session *s, uint32_t ip, uint16_t port, rc_msg *m){
    uint8_t b[RC_MAX_DGRAM]; int n = rc_encode(m, b, sizeof b);
    if (n <= 0 || !s->io.send || s->closed) return;
    s->io.send(s->io.user, ip, port, b, n); s->tx++; s->tx_bytes += (uint32_t)n;
}
static void tx(rc_session *s, const rc_peer *p, rc_msg *m){ tx_to(s, p->ip, p->port, m); }
/* the hub decides who our peers are: one is configured, and it answered
 * lately (or we are still in the first 10 s, waiting for its first list) */
static int hub_mode(const rc_session *s, int64_t now){
    if (!s->conf.hub_ip) return 0;
    if (!s->hub_since) return 1;
    return now - (s->hub_heard_us ? s->hub_heard_us : s->hub_since) < T_HUB_SILENT;
}
int rc_send_duel(rc_session *s, uint32_t to, uint8_t op, uint8_t arg, uint32_t duel){
    for (int i = 0; i < RC_TABLE; i++){
        rc_peer *p = &s->peer[i];
        if (p->state != RC_PS_EST || p->id != to || !p->duels) continue;
        rc_msg m; hdr(s, &m, RC_MSG_DUEL);
        m.u.duel.op = op; m.u.duel.arg = arg; m.u.duel.to = to; m.u.duel.duel = duel;
        uint32_t before = s->tx;
        tx(s, p, &m);
        return s->tx != before;
    }
    return 0;
}
int rc_take_duel(rc_session *s, uint32_t *from, uint8_t *op, uint8_t *arg, uint32_t *duel){
    if (!s->n_duel_in) return 0;
    *from = s->duel_in[0].from; *op = s->duel_in[0].op; *arg = s->duel_in[0].arg; *duel = s->duel_in[0].duel;
    for (int k = 1; k < s->n_duel_in; k++) s->duel_in[k - 1] = s->duel_in[k];
    s->n_duel_in--;
    return 1;
}
int rc_send_lobby(rc_session *s, const rc_lobby *l){
    if (s->closed || !s->conf.lobbies) return 0;
    rc_msg m; hdr(s, &m, RC_MSG_LOBBY);
    m.u.lobby = *l;
    uint32_t before = s->tx;
    if (!l->to){
        if (!s->conf.hub_ip || !s->hub_heard_us) return 0;
        tx_to(s, s->conf.hub_ip, s->conf.hub_port, &m);
        return s->tx != before;
    }
    for (int i = 0; i < RC_TABLE; i++){
        rc_peer *p = &s->peer[i];
        if (p->state != RC_PS_EST || p->id != l->to || !p->lobbies) continue;
        tx(s, p, &m);
        return s->tx != before;
    }
    return 0;
}
int rc_send_racerep(rc_session *s, const rc_racerep *r){
    if (s->closed || !s->conf.hub_ip || !s->hub_heard_us) return 0;
    rc_msg m; hdr(s, &m, RC_MSG_RACEREP);
    m.u.racerep = *r;
    uint32_t before = s->tx;
    tx_to(s, s->conf.hub_ip, s->conf.hub_port, &m);
    return s->tx != before;
}
int rc_send_raceres(rc_session *s, const rc_raceres *r){
    if (s->closed || !s->conf.hub_ip || !s->hub_heard_us) return 0;
    rc_msg m; hdr(s, &m, RC_MSG_RACERES);
    m.u.raceres = *r;
    uint32_t before = s->tx;
    tx_to(s, s->conf.hub_ip, s->conf.hub_port, &m);
    return s->tx != before;
}
int rc_take_lobby(rc_session *s, uint32_t *from, rc_lobby *l){
    if (!s->n_lobby_in) return 0;
    *from = s->lobby_in[0].from; *l = s->lobby_in[0].l;
    for (int k = 1; k < s->n_lobby_in; k++) s->lobby_in[k - 1] = s->lobby_in[k];
    s->n_lobby_in--;
    return 1;
}
int rc_send_say(rc_session *s, uint8_t phrase, uint8_t seq){
    if (s->closed || !s->conf.say) return 0;
    int n = 0;
    for (int i = 0; i < RC_TABLE; i++){
        rc_peer *p = &s->peer[i];
        if (p->state != RC_PS_EST || !p->say) continue;
        rc_msg m; hdr(s, &m, RC_MSG_SAY);
        m.u.say.phrase = phrase; m.u.say.seq = seq;
        uint32_t before = s->tx;
        tx(s, p, &m);
        n += s->tx != before;
    }
    return n;
}
int rc_take_say(rc_session *s, uint32_t *from, uint8_t *phrase, uint8_t *seq){
    if (!s->n_say_in) return 0;
    *from = s->say_in[0].from; *phrase = s->say_in[0].phrase; *seq = s->say_in[0].seq;
    for (int k = 1; k < s->n_say_in; k++) s->say_in[k - 1] = s->say_in[k];
    s->n_say_in--;
    return 1;
}
/* the hub's open lobbies, with the answer to our last map question */
static void lobbies_from_hub(rc_session *s, const rc_msg *m){
    /* (the MAP before it has answered the question already: map_asked_us is 0 by now) */
    if (!s->map_req || m->u.lobbies.req != s->map_req || m->u.lobbies.req == s->lobbies_req || !s->conf.lobbies) return;
    s->lobbies_req = m->u.lobbies.req;
    int n = 0;
    for (int k = 0; k < m->u.lobbies.n; k++){
        const uint8_t *e = m->u.lobbies.data + (size_t)k * RC_LOBBIES_ENTRY;
        uint32_t race = (uint32_t)e[4] | (uint32_t)e[5] << 8 | (uint32_t)e[6] << 16 | (uint32_t)e[7] << 24;
        if (!race || e[9] < 2 || e[9] > RC_LOBBY_MAX || e[8] > e[9] || e[10] >= RC_LS__N) continue;
        s->lobbies_e[n].x = (int16_t)(e[0] | e[1] << 8); s->lobbies_e[n].y = (int16_t)(e[2] | e[3] << 8);
        s->lobbies_e[n].race = race; s->lobbies_e[n].n = e[8]; s->lobbies_e[n].max = e[9]; s->lobbies_e[n].state = e[10];
        n++;
    }
    s->lobbies_n = n; s->lobbies_gen++;
}
int rc_ask_map(rc_session *s, int64_t now){
    if (s->closed || !s->conf.hub_ip || !s->hub_heard_us || !hub_mode(s, now)) return 0;
    rc_msg m; hdr(s, &m, RC_MSG_MAPREQ);
    m.u.mapreq.req = ++s->map_req;
    s->map_mask = 0; s->map_nparts = 0; s->map_asked_us = now;
    tx_to(s, s->conf.hub_ip, s->conf.hub_port, &m);
    return 1;
}
/* a part of the hub's answer to our last question: a complete answer
 * replaces the last one */
static void map_part(rc_session *s, const rc_msg *m){
    if (m->u.map.req != s->map_req || !s->map_asked_us) return;
    if (s->map_mask && (m->u.map.nparts != s->map_nparts || m->u.map.total != s->map_asm_total)) s->map_mask = 0;   /* not the same answer */
    s->map_nparts = m->u.map.nparts; s->map_asm_total = m->u.map.total;
    memcpy(s->map_asm + (size_t)m->u.map.part * RC_MAP_PER * RC_MAP_ENTRY, m->u.map.data, (size_t)m->u.map.n * RC_MAP_ENTRY);
    s->map_part_n[m->u.map.part] = m->u.map.n;
    s->map_mask |= (uint8_t)(1u << m->u.map.part);
    if (s->map_mask != (1u << s->map_nparts) - 1u) return;
    int n = 0;
    for (int j = 0; j < s->map_nparts; j++)
        for (int k = 0; k < s->map_part_n[j]; k++){
            const uint8_t *e = s->map_asm + ((size_t)j * RC_MAP_PER + (size_t)k) * RC_MAP_ENTRY;
            s->map_e[n].x = (int16_t)(e[0] | e[1] << 8); s->map_e[n].y = (int16_t)(e[2] | e[3] << 8); s->map_e[n].heading = e[4];
            n++;
        }
    s->map_n = n; s->map_total = s->map_asm_total; s->map_gen++;
    s->map_mask = 0; s->map_asked_us = 0;                    /* answered: a repeat of a part changes nothing */
}
int rc_established(const rc_session *s){ int n = 0; for (int i = 0; i < RC_TABLE; i++) if (s->peer[i].state == RC_PS_EST) n++; return n; }
/* an established peer counts for the group (max_players) unless the hub
 * decides and its last list left it out: it goes within T_HUB_GRACE, and a
 * player the hub lists instead does not wait for that */
static int in_group(const rc_session *s, const rc_peer *p){
    return s->hub_was != 2 || p->configured || p->hub_listed_us == s->hub_heard_us;
}
int rc_peer_listed(const rc_session *s, int i){ return i >= 0 && i < RC_TABLE && in_group(s, &s->peer[i]); }
void rc_set_crews(rc_session *s, const uint32_t *crews, int n){
    s->conf.n_crews = 0;
    for (int i = 0; i < n && s->conf.n_crews < RC_CREW_MAX; i++){
        int dup = !crews[i];
        for (int k = 0; k < s->conf.n_crews; k++) dup |= s->conf.crews[k] == crews[i];
        if (!dup) s->conf.crews[s->conf.n_crews++] = crews[i];
    }
    for (int k = s->conf.n_crews; k < RC_CREW_MAX; k++) s->conf.crews[k] = 0;
    /* HELLOs due at the next tick: the hub links us by them (and marks us
     * for our crew-mates); without a hub, a peer colours us by them */
    s->hub_hello_tx = INT64_MIN / 4;
    if (!s->conf.hub_ip) for (int i = 0; i < RC_TABLE; i++) if (s->peer[i].state != RC_PS_FREE) s->peer[i].last_hello_tx = INT64_MIN / 4;
}
int rc_peer_crew(const rc_session *s, int i){
    if (i < 0 || i >= RC_TABLE) return -1;
    const rc_peer *p = &s->peer[i];
    for (int c = 0; c < s->conf.n_crews && c < RC_CREW_MAX; c++){
        for (int k = 0; k < p->n_crews && k < RC_CREW_MAX; k++)
            if (s->conf.crews[c] && p->crews[k] == s->conf.crews[c]) return c;
        for (int k = 0; k < p->n_hub_crews && k < RC_CREW_MAX; k++)
            if (s->conf.crews[c] && p->hub_crews[k] == s->conf.crews[c]) return c;
    }
    return -1;
}
static int group_count(const rc_session *s){
    int n = 0;
    for (int i = 0; i < RC_TABLE; i++) if (s->peer[i].state == RC_PS_EST && in_group(s, &s->peer[i])) n++;
    return n;
}
/* another table entry (not `except`) already has this sender id */
static int id_elsewhere(const rc_session *s, uint32_t id, int except){
    for (int i = 0; i < RC_TABLE; i++) if (i != except && s->peer[i].state != RC_PS_FREE && s->peer[i].id == id) return 1;
    return 0;
}
int rc_addr_private(uint32_t ip){
    return (ip >> 24) == 127 || (ip >> 24) == 10 || (ip >> 20) == ((172u << 4) | 1u) ||
           (ip >> 16) == ((192u << 8) | 168u) || (ip >> 16) == ((169u << 8) | 254u);
}
static int allowed(const rc_session *s, uint32_t ip){
    if (s->conf.test_mode) return (ip >> 24) == 127;
    return rc_addr_private(ip);
}
void rc_session_init(rc_session *s, const rc_config *cfg, const rc_io *io){
    memset(s, 0, sizeof *s);
    s->conf = *cfg; s->io = *io;
    if (s->conf.max_players < 2) s->conf.max_players = 2;
    if (s->conf.max_players > RC_MAX_PLAYERS) s->conf.max_players = RC_MAX_PLAYERS;
    if (s->conf.rate_hz < 10) s->conf.rate_hz = 10;
    if (s->conf.rate_hz > 60) s->conf.rate_hz = 60;
    if (s->conf.snap_states < 1 || s->conf.snap_states > RC_SNAP_STATES) s->conf.snap_states = RC_SNAP_STATES;
    if (!s->conf.self_id) s->conf.self_id = 1;
    rc_name_clean(s->conf.name, cfg->name, RC_NAME_MAX + 1);
    for (int i = 0; i < RC_TABLE; i++) rc_interp_reset(&s->peer[i].mo);
    s->status = RC_ST_MENUS;
    s->last_beacon = -T_BEACON;
}
static int find(rc_session *s, uint32_t ip, uint16_t port){
    for (int i = 0; i < RC_TABLE; i++) if (s->peer[i].state != RC_PS_FREE && s->peer[i].ip == ip && s->peer[i].port == port) return i;
    return -1;
}
static void peer_clear(rc_peer *p, int keep_addr){
    uint32_t ip = p->ip; uint16_t port = p->port; int conf = p->configured; int64_t added = p->added_us;
    memset(p, 0, sizeof *p); rc_interp_reset(&p->mo);
    if (keep_addr){ p->ip = ip; p->port = port; p->configured = conf; p->added_us = added; p->state = RC_PS_CANDIDATE; p->last_hello_tx = -T_HELLO_CAND; }
}
int rc_add_peer(rc_session *s, uint32_t ip, uint16_t port, int configured, int64_t now_us){
    if (!ip || !port) return -1;
    if (!configured && !allowed(s, ip)) return -1;
    if (s->conf.test_mode && (ip >> 24) != 127) return -1;
    int i = find(s, ip, port);
    if (i >= 0){ if (configured) s->peer[i].configured = 1; return i; }
    for (i = 0; i < RC_TABLE; i++) if (s->peer[i].state == RC_PS_FREE) break;
    if (i == RC_TABLE){ s->rx_unknown_full++; return -1; }
    rc_peer *p = &s->peer[i];
    peer_clear(p, 0);
    p->state = RC_PS_CANDIDATE; p->ip = ip; p->port = port; p->configured = configured; p->gossiped = !configured;
    p->added_us = now_us; p->last_hello_tx = now_us - T_HELLO_CAND;
    return i;
}
/* `to`: the peer it goes to (none for the beacon and the hub); crews: with
 * our crews (the hub; a peer that reads them, but only without a hub: with
 * one, the hub marks our crew-mates on their lists, and no peer learns our
 * crews) */
static void fill_hello(rc_session *s, rc_msg *m, const rc_peer *to, int crews){
    hdr(s, m, RC_MSG_HELLO);
    m->h.flags = RC_HF_STATEX | RC_HF_CREW | RC_HF_HUBCREW;
    if (crews && (!to || !s->conf.hub_ip)){
        int n = s->conf.n_crews < 0 ? 0 : s->conf.n_crews > RC_CREW_MAX ? RC_CREW_MAX : s->conf.n_crews;
        for (int i = 0; i < n; i++) if (s->conf.crews[i]) m->u.hello.crews[m->u.hello.n_crews++] = s->conf.crews[i];
    }
    if (s->conf.duels) m->h.flags |= RC_HF_DUELS;
    if (s->conf.lobbies) m->h.flags |= RC_HF_LOBBY;
    if (s->conf.say) m->h.flags |= RC_HF_SAY;
    if (!to) m->h.flags |= RC_HF_ALL;
    m->u.hello.core_ver = RC_CORE_VER; m->u.hello.platform = s->conf.platform; m->u.hello.status = s->status;
    m->u.hello.listen_port = s->conf.port; m->u.hello.cfg_rev = s->cfg_rev; m->u.hello.room = s->conf.room;
    memcpy(m->u.hello.name, s->conf.name, sizeof m->u.hello.name);
    int n = 0;
    /* a core of version 1 (the beacon and the hub may reach one too) reads 7
     * ids at most, and drops a longer HELLO */
    int cap = to && to->core_ver >= 2 ? RC_MAX_PEERS : RC_PEERS_V1;
    /* the ids we hear: our group first, then the established ones the hub's
     * last list left out (they are going), then the others that spoke lately;
     * with the group full not those (a peer that sees its id here takes us as
     * established: with a larger max_players of its own it would join
     * one-sidedly, and never get a snapshot) */
    int full = group_count(s) + 1 >= s->conf.max_players;
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < RC_TABLE && n < cap; i++){
            const rc_peer *p = &s->peer[i];
            if (!p->id || !p->last_rx_us) continue;
            if (pass < 2 && (p->state != RC_PS_EST || (pass == 0) != in_group(s, p))) continue;
            if (pass == 2 && (full || p->state != RC_PS_HEARD)) continue;
            m->u.hello.seen[n++] = p->id;
        }
    /* more than the list holds (the hub's links overlap for a moment while
     * one goes and another comes): the peer it goes to always finds its id,
     * or it would take it that we no longer hear it */
    if (to && to->id && to->last_rx_us && (to->state == RC_PS_EST || (!full && to->state == RC_PS_HEARD))){
        int have = 0;
        for (int k = 0; k < n; k++) if (m->u.hello.seen[k] == to->id) have = 1;
        if (!have){ if (n < cap) n++; m->u.hello.seen[n - 1] = to->id; }
    }
    m->u.hello.n_seen = (uint8_t)n;
}
static void send_hello(rc_session *s, rc_peer *p, int64_t now){ rc_msg m; fill_hello(s, &m, p, p->reads_crews); tx(s, p, &m); p->last_hello_tx = now; }
static void send_cfg(rc_session *s, rc_peer *p, uint8_t have_mask){
    if (!s->cfg_len) return;
    int nf = (s->cfg_len + RC_CFG_FRAG - 1) / RC_CFG_FRAG;
    for (int f = 0; f < nf; f++){
        if (have_mask & (1u << f)) continue;
        rc_msg m; hdr(s, &m, RC_MSG_CFG);
        m.u.cfg.cfg_rev = s->cfg_rev; m.u.cfg.frag = (uint8_t)f; m.u.cfg.nfrag = (uint8_t)nf; m.u.cfg.total = (uint16_t)s->cfg_len;
        m.u.cfg.dlen = (uint16_t)(f + 1 < nf ? RC_CFG_FRAG : s->cfg_len - f * RC_CFG_FRAG);
        m.u.cfg.data = s->cfg + f * RC_CFG_FRAG;
        tx(s, p, &m);
    }
}
/* the record to a peer that asked for it, or joined: at most every
 * T_CFG_ANSWER (it asks every T_CFGREQ), so that a peer asking without end (a
 * 23-byte request for up to 2 KB) cannot spend this player's link on answers
 * (the security review: past the switch's limit its car froze for everyone) */
static void answer_cfg(rc_session *s, rc_peer *p, uint8_t have_mask, int64_t now){
    if (now < p->cfg_answer_us && p->cfg_answer_us - now <= T_CFG_ANSWER) return;
    p->cfg_answer_us = now + T_CFG_ANSWER;
    send_cfg(s, p, have_mask);
}
static void drop_peer(rc_session *s, int i, const char *why){
    rc_peer *p = &s->peer[i];
    if (p->state == RC_PS_EST) note(s, RC_EV_LEAVE, i, p->name, " left: ", why);
    s->peers_dirty = 1;
    peer_clear(p, p->configured);
}
static void want_cfg(rc_session *s, rc_peer *p, uint16_t rev, int64_t now){
    if (p->state != RC_PS_EST || (p->cfg_len && p->cfg_rev == rev) || !rev || rev == p->cfg_bad_rev) return;
    if (now - p->last_cfgreq_tx < T_CFGREQ) return;
    rc_msg m; hdr(s, &m, RC_MSG_CFG_REQ);
    m.u.cfgreq.cfg_rev = rev; m.u.cfgreq.have_mask = p->asm_rev == rev ? p->asm_mask : 0;
    tx(s, p, &m); p->last_cfgreq_tx = now;
}
static int rate_ok(rc_peer *p, int len, int64_t now){
    if (now - p->rl_t0 >= 1000000 || now < p->rl_t0){ p->rl_t0 = now; p->rl_n = 0; p->rl_bytes = 0; }
    p->rl_n++; p->rl_bytes += len;
    return p->rl_n <= RL_DGRAMS && p->rl_bytes <= RL_BYTES;
}
void rc_receive(rc_session *s, uint32_t ip, uint16_t port, const uint8_t *buf, int len, int64_t now){
    s->rx++;
    if (!allowed(s, ip)){
        /* outside the private ranges only a peer the user listed (a VPN
         * overlay address, say) is heard */
        int k = find(s, ip, port);
        if (k < 0 || !s->peer[k].configured){ s->rx_policy++; return; }
    }
    rc_msg m; int e = rc_decode(buf, len, &m);
    if (e == RC_E_VERSION){
        /* another protocol version: nothing is answered, but the log says
         * so (every 30 s at most), or two players on different versions
         * would just never see each other */
        s->rx_version++;
        if (!s->noted_version || now - s->note_version_us >= T_MISMATCH_NOTE){
            char t[150]; uint32_t n = 0;
            cat(t, &n, sizeof t, "a player at "); cat_addr(t, &n, sizeof t, ip, port);
            cat(t, &n, sizeof t, " speaks protocol version "); cat_u(t, &n, sizeof t, buf[4]);
            cat(t, &n, sizeof t, ", this game version "); cat_u(t, &n, sizeof t, RC_PROTO_VER);
            cat(t, &n, sizeof t, ": both need the same free-roam version");
            note(s, RC_EV_NOTE, find(s, ip, port), t, "", "");
            s->noted_version = 1; s->note_version_us = now;
        }
        return;
    }
    if (e){ s->rx_bad++; return; }
    if (m.h.sender == s->conf.self_id) return;                 /* our own beacon */
    if (s->conf.hub_ip && ip == s->conf.hub_ip && port == s->conf.hub_port){
        /* the hub's list of the players near us: each one is a candidate
         * (the hub lists us on theirs at the same time), and stays while it
         * is listed (rc_tick drops the others) */
        if (m.h.type == RC_MSG_MAP){ map_part(s, &m); return; }
        if (m.h.type == RC_MSG_LOBBIES){ lobbies_from_hub(s, &m); return; }
        if (m.h.type != RC_MSG_PEERS) return;
        s->hub_heard_us = now; s->hub_lists++;
        int marks = (m.h.flags & RC_HF_CREW) != 0;
        for (int k = 0; k < m.u.peers.n; k++){
            uint32_t id = m.u.peers.e[k].id;
            if (id == s->conf.self_id) continue;
            int j = find(s, m.u.peers.e[k].ip, m.u.peers.e[k].port);
            if (j < 0){
                if (id_elsewhere(s, id, -1)) continue;
                j = rc_add_peer(s, m.u.peers.e[k].ip, m.u.peers.e[k].port, 0, now);
                if (j < 0) continue;
            }
            rc_peer *q = &s->peer[j];
            q->hub_listed_us = now;
            /* the crews of ours it is in, as the hub marks them (none: a list without marks) */
            q->n_hub_crews = 0;
            for (int c = 0; marks && c < m.u.peers.n_mine; c++)
                if (m.u.peers.crew[k] >> c & 1u) q->hub_crews[q->n_hub_crews++] = m.u.peers.mine[c];
        }
        return;
    }
    for (int k = 0; k < s->nbye; k++)
        if (s->bye[k].id == m.h.sender && now - s->bye[k].at < 3000000){ s->rx_after_bye++; return; }
    int i = find(s, ip, port);
    if (i < 0){
        if (m.h.type != RC_MSG_HELLO) return;
        /* with a hub, only the players it lists for us (they arrive with
         * its next list, within a second) */
        if (hub_mode(s, now)){ s->rx_unlisted++; return; }
        if (m.u.hello.room != s->conf.room){
            s->rx_room++;
            if (!s->noted_room || now - s->note_room_us >= T_MISMATCH_NOTE){
                char t[150]; uint32_t n = 0;
                cat(t, &n, sizeof t, m.u.hello.name); cat(t, &n, sizeof t, " at "); cat_addr(t, &n, sizeof t, ip, port);
                cat(t, &n, sizeof t, " is in another room (the Room setting): you do not see each other");
                note(s, RC_EV_NOTE, -1, t, "", "");
                s->noted_room = 1; s->note_room_us = now;
            }
            return;
        }
        /* a player we know already, heard from a second address (two
         * interfaces, a VPN next to the LAN): the first address stays */
        if (id_elsewhere(s, m.h.sender, -1)){ s->rx_dup_id++; return; }
        i = rc_add_peer(s, ip, port, 0, now);
        if (i < 0) return;
        s->peer[i].gossiped = 0;
    }
    rc_peer *p = &s->peer[i];
    if (!rate_ok(p, len, now)){ s->rx_ratelimit++; p->rx_drop++; return; }
    if (!p->id && id_elsewhere(s, m.h.sender, i)){ s->rx_dup_id++; return; }
    if (p->id && p->id != m.h.sender){
        /* another id from a known address: only a HELLO says the player
         * restarted; anything else is a stray datagram */
        if (m.h.type != RC_MSG_HELLO){ s->rx_stray++; return; }
        if (id_elsewhere(s, m.h.sender, i)){ s->rx_dup_id++; return; }
        note(s, RC_EV_RESTART, i, p->name, " restarted", "");
        int conf = p->configured;
        drop_peer(s, i, "restart");
        p->state = RC_PS_CANDIDATE; p->configured = conf; p->ip = ip; p->port = port; p->added_us = now;
    }
    if (!p->id) p->id = m.h.sender;
    p->rx++; p->last_rx_us = now; p->lost = 0;
    switch (m.h.type){
    case RC_MSG_HELLO: {
        if (m.u.hello.room != s->conf.room){ s->rx_room++; if (p->state == RC_PS_EST) drop_peer(s, i, "other room"); return; }
        int first = !p->name[0];
        memcpy(p->name, m.u.hello.name, sizeof p->name);
        p->platform = m.u.hello.platform; p->status = m.u.hello.status; p->cfg_rev_seen = m.u.hello.cfg_rev;
        p->core_ver = m.u.hello.core_ver; p->duels = (m.h.flags & RC_HF_DUELS) != 0;
        p->lobbies = (m.h.flags & RC_HF_LOBBY) != 0;
        p->statex = (m.h.flags & RC_HF_STATEX) != 0;
        p->say = (m.h.flags & RC_HF_SAY) != 0;
        int was = p->reads_crews;
        p->reads_crews = (m.h.flags & RC_HF_CREW) != 0;
        p->n_crews = m.u.hello.n_crews;
        memcpy(p->crews, m.u.hello.crews, sizeof p->crews);
        int heard = 0;
        for (int k = 0; k < m.u.hello.n_seen; k++) if (m.u.hello.seen[k] == s->conf.self_id) heard = 1;
        /* the beacon of a group past 8: a full list without our id (RC_HF_ALL) */
        int vague = !heard && (m.h.flags & RC_HF_ALL) && m.u.hello.n_seen >= RC_PEERS_V1;
        if (!(vague && p->state == RC_PS_EST)) p->hears_us = heard;
        if (p->hears_us) p->hears_miss = 0;
        if (p->state != RC_PS_EST){
            if (p->state == RC_PS_CANDIDATE) p->state = RC_PS_HEARD;
            if (p->hears_us){
                if (group_count(s) + 1 >= s->conf.max_players){
                    if (first) note(s, RC_EV_NOTE, i, p->name, ": the group is full", "");
                } else {
                    p->state = RC_PS_EST; s->peers_dirty = 1;
                    note(s, RC_EV_JOIN, i, p->name, " joined", "");
                    send_hello(s, p, now);                       /* so it learns at once */
                    answer_cfg(s, p, 0, now);
                }
            } else if (now - p->last_hello_tx > 100000) send_hello(s, p, now);   /* answer: now we hear it */
        } else if (!p->hears_us && !vague){
            if (s->hub_was == 2 && !p->hears_miss){
                /* with the hub, once: the hub may have just linked us again
                 * after the peer let us go, and it hears us at our answer */
                p->hears_miss = 1; send_hello(s, p, now);
            } else {
                p->state = RC_PS_HEARD; s->peers_dirty = 1;
                note(s, RC_EV_LEAVE, i, p->name, " left: ", "no longer hears us");
            }
        }
        /* it reads crews now: ours at once (the HELLOs before had none); with a hub, it hears of them from the hub */
        if (!was && p->reads_crews && s->conf.n_crews && !s->conf.hub_ip && p->state != RC_PS_CANDIDATE && now - p->last_hello_tx > 100000)
            send_hello(s, p, now);
        want_cfg(s, p, p->cfg_rev_seen, now);
        break; }
    case RC_MSG_PEERS:
        if (p->state != RC_PS_EST || hub_mode(s, now)) return;      /* with a hub, only its list */
        for (int k = 0; k < m.u.peers.n; k++){
            if (m.u.peers.e[k].id == s->conf.self_id) continue;
            int j = find(s, m.u.peers.e[k].ip, m.u.peers.e[k].port);
            if (j < 0 && !id_elsewhere(s, m.u.peers.e[k].id, -1)) rc_add_peer(s, m.u.peers.e[k].ip, m.u.peers.e[k].port, 0, now);
        }
        break;
    case RC_MSG_SNAP:
        if (p->state != RC_PS_EST) return;
        p->rx_snap++; p->last_snap_rx = now; p->status = m.u.snap.status; p->cfg_rev_seen = m.u.snap.cfg_rev;
        /* the sender's snapshot sequence: a gap is snapshots lost on the way */
        if (p->seq_init){
            uint16_t d = (uint16_t)(m.h.seq - p->last_seq), every = (uint16_t)(m.h.flags & RC_HF_EVERY);
            if (every < 1) every = 1;                                      /* far away: every n-th (rc_push_state) */
            if (d > every && d < 0x8000u) p->seq_lost += d - every;
            if (d && d < 0x8000u) p->last_seq = m.h.seq;
        }
        else { p->seq_init = 1; p->last_seq = m.h.seq; }
        for (int k = m.u.snap.n - 1; k >= 0; k--)
            rc_interp_insert(&p->mo, &m.u.snap.s[k], m.u.snap.epoch, m.u.snap.clock, m.u.snap.status, now, k == 0);
        want_cfg(s, p, m.u.snap.cfg_rev, now);
        break;
    case RC_MSG_CFG: {
        if (p->state != RC_PS_EST) return;
        if (p->cfg_len && p->cfg_rev == m.u.cfg.cfg_rev) return;     /* have it */
        if (p->cfg_bad_rev && m.u.cfg.cfg_rev == p->cfg_bad_rev) return;   /* cannot read it */
        if (p->asm_rev != m.u.cfg.cfg_rev || p->asm_nfrag != m.u.cfg.nfrag || p->asm_total != m.u.cfg.total){
            p->asm_rev = m.u.cfg.cfg_rev; p->asm_nfrag = m.u.cfg.nfrag; p->asm_total = m.u.cfg.total; p->asm_mask = 0; }
        memcpy(p->asm_buf + (uint32_t)m.u.cfg.frag * RC_CFG_FRAG, m.u.cfg.data, m.u.cfg.dlen);
        p->asm_mask |= (uint8_t)(1u << m.u.cfg.frag);
        if (p->asm_mask == (uint8_t)((1u << p->asm_nfrag) - 1u)){
            rc_carcfg chk;
            if (rc_carcfg_decode(p->asm_buf, p->asm_total, &chk) == RC_OK){
                memcpy(p->cfg, p->asm_buf, p->asm_total); p->cfg_len = p->asm_total; p->cfg_rev = p->asm_rev; p->cfg_gen++;
                note(s, RC_EV_CFG, i, p->name, "'s car: ", chk.type_name);
            } else {
                /* not asked for again until the player's car changes (a new
                 * revision): a record of a newer game side would otherwise
                 * be requested, and logged, twice a second for good */
                char t[96]; uint32_t n = 0;
                cat(t, &n, sizeof t, ": car record not understood (record version "); cat_u(t, &n, sizeof t, p->asm_buf[0]);
                cat(t, &n, sizeof t, "): the car is not shown");
                s->rx_bad++; p->cfg_bad_rev = p->asm_rev;
                note(s, RC_EV_NOTE, i, p->name, t, "");
            }
            p->asm_mask = 0; p->asm_rev = 0;
        }
        break; }
    case RC_MSG_CFG_REQ:
        if (p->state != RC_PS_EST) return;
        answer_cfg(s, p, m.u.cfgreq.cfg_rev == s->cfg_rev ? m.u.cfgreq.have_mask : 0, now);
        break;
    case RC_MSG_PING: {
        if (p->state != RC_PS_EST) return;
        rc_msg r; hdr(s, &r, RC_MSG_PONG);
        r.u.ping.t0 = m.u.ping.t0; r.u.ping.t1 = (uint32_t)now; r.u.ping.t2 = (uint32_t)now;
        tx(s, p, &r);
        break; }
    case RC_MSG_PONG:
        p->rtt_us = (int32_t)((uint32_t)now - m.u.ping.t0 - (m.u.ping.t2 - m.u.ping.t1));
        break;
    case RC_MSG_DUEL:
        if (p->state != RC_PS_EST || m.u.duel.to != s->conf.self_id) return;
        if (s->n_duel_in == RC_DUEL_INBOX){ s->duel_dropped++; return; }
        s->duel_in[s->n_duel_in].from = m.h.sender; s->duel_in[s->n_duel_in].op = m.u.duel.op;
        s->duel_in[s->n_duel_in].arg = m.u.duel.arg; s->duel_in[s->n_duel_in].duel = m.u.duel.duel;
        s->n_duel_in++;
        break;
    case RC_MSG_LOBBY:
        if (p->state != RC_PS_EST || m.u.lobby.to != s->conf.self_id || !s->conf.lobbies) return;
        if (s->n_lobby_in == RC_LOBBY_INBOX){ s->lobby_dropped++; return; }
        s->lobby_in[s->n_lobby_in].from = m.h.sender; s->lobby_in[s->n_lobby_in].l = m.u.lobby;
        s->n_lobby_in++;
        break;
    case RC_MSG_SAY:
        if (p->state != RC_PS_EST || !s->conf.say) return;
        if (s->n_say_in == RC_SAY_INBOX){ s->say_dropped++; return; }
        s->say_in[s->n_say_in].from = m.h.sender; s->say_in[s->n_say_in].phrase = m.u.say.phrase;
        s->say_in[s->n_say_in].seq = m.u.say.seq;
        s->n_say_in++;
        break;
    case RC_MSG_BYE: {
        int k = 0;
        if (s->nbye < 4) k = s->nbye++;
        else for (int j = 1; j < 4; j++) if (s->bye[j].at < s->bye[k].at) k = j;      /* the oldest goes */
        s->bye[k].id = m.h.sender; s->bye[k].at = now;
        drop_peer(s, i, m.u.bye.reason == 1 ? "switched off" : "quit");
        break; }
    }
}
void rc_tick(rc_session *s, int64_t now){
    if (s->closed) return;
    int hub = hub_mode(s, now);
    if (s->conf.hub_ip){
        if (!s->hub_since) s->hub_since = now ? now : 1;
        if (now - s->hub_hello_tx >= T_HUB_HELLO){ rc_msg m; fill_hello(s, &m, 0, 1); tx_to(s, s->conf.hub_ip, s->conf.hub_port, &m); s->hub_hello_tx = now; }
        int st = !hub ? 0 : s->hub_heard_us ? 2 : 1;        /* none or silent, waiting for its first list, answering */
        if (st != s->hub_was && st != 1){
            if (st == 2) note(s, RC_EV_NOTE, -1, "the server's free-roam hub answers: the players near you come from its list", "", "");
            else note(s, RC_EV_NOTE, -1, s->hub_heard_us ? "the server's free-roam hub went silent" : "no free-roam hub on this server",
                      s->conf.discovery ? ": players are found by broadcast" : "", "");
        }
        s->hub_was = st;
    }
    if (s->conf.discovery && !hub && now - s->last_beacon >= T_BEACON && s->io.broadcast){
        rc_msg m; fill_hello(s, &m, 0, 0);                         /* (the beacon: no crews) */
        uint8_t b[RC_MAX_DGRAM]; int n = rc_encode(&m, b, sizeof b);
        if (n > 0) s->io.broadcast(s->io.user, s->conf.port, b, n);
        s->last_beacon = now;
    }
    for (int i = 0; i < RC_TABLE; i++){
        rc_peer *p = &s->peer[i];
        if (p->state == RC_PS_FREE) continue;
        if (p->last_rx_us && now - p->last_rx_us > T_DROP){ drop_peer(s, i, "timeout"); continue; }
        /* lists keep coming without it (a silent hub drops nobody) */
        if (hub && s->hub_heard_us && !p->configured && s->hub_heard_us - p->hub_listed_us > T_HUB_GRACE){ drop_peer(s, i, "out of range"); continue; }
        if (!p->last_rx_us && !p->configured && now - p->added_us > T_GOSSIP_FORGET){ peer_clear(p, 0); continue; }
        if (p->state == RC_PS_EST && now - p->last_rx_us > T_LOST && !p->lost){
            p->lost = 1; note(s, RC_EV_NOTE, i, p->name, ": no packets for 3 s", ""); }
        int64_t every = p->state == RC_PS_EST ? T_HELLO_EST : T_HELLO_CAND;
        if (now - p->last_hello_tx >= every) send_hello(s, p, now);
        if (p->state == RC_PS_EST && now - p->last_ping_tx >= T_PING){
            rc_msg m; hdr(s, &m, RC_MSG_PING); m.u.ping.t0 = (uint32_t)now; tx(s, p, &m); p->last_ping_tx = now; }
        if (p->state == RC_PS_EST) want_cfg(s, p, p->cfg_rev_seen, now);
    }
    if (!hub && (s->peers_dirty || now - s->last_peers_tx >= T_PEERS)){
        rc_msg m; hdr(s, &m, RC_MSG_PEERS);
        int n = 0;
        for (int i = 0; i < RC_TABLE && n < RC_MAX_PEERS; i++){
            const rc_peer *p = &s->peer[i];
            if (p->state != RC_PS_EST) continue;
            m.u.peers.e[n].ip = p->ip; m.u.peers.e[n].port = p->port; m.u.peers.e[n].id = p->id; n++;
        }
        /* a core of version 1 gets the first 7 (it drops a longer list) */
        for (int i = 0; i < RC_TABLE && n; i++) if (s->peer[i].state == RC_PS_EST){
            m.u.peers.n = (uint8_t)(s->peer[i].core_ver >= 2 || n < RC_PEERS_V1 ? n : RC_PEERS_V1);
            tx(s, &s->peer[i], &m);
        }
        s->peers_dirty = 0; s->last_peers_tx = now;
    }
}
void rc_set_status(rc_session *s, uint8_t st){
    if (st >= RC_ST__N || st == s->status) return;
    s->status = st;
    /* tell the others now, not at the next HELLO: they hide or show our car */
    for (int i = 0; i < RC_TABLE; i++) if (s->peer[i].state == RC_PS_EST){ rc_msg m; fill_hello(s, &m, &s->peer[i], s->peer[i].reads_crews); tx(s, &s->peer[i], &m); }
    if (s->conf.hub_ip){ rc_msg m; fill_hello(s, &m, 0, 1); tx_to(s, s->conf.hub_ip, s->conf.hub_port, &m); }
}
void rc_bump_epoch(rc_session *s){ s->epoch++; s->nlast = 0; }
void rc_bump_clock(rc_session *s){ s->clock++; s->nlast = 0; }
void rc_set_cfg(rc_session *s, const uint8_t *rec, int len, int64_t now){
    (void)now;
    if (len <= 0 || len > RC_CFG_MAX) return;
    if (len == s->cfg_len && !memcmp(rec, s->cfg, (size_t)len)) return;
    memcpy(s->cfg, rec, (size_t)len); s->cfg_len = len;
    s->cfg_rev++; if (!s->cfg_rev) s->cfg_rev = 1;
    for (int i = 0; i < RC_TABLE; i++) if (s->peer[i].state == RC_PS_EST) send_cfg(s, &s->peer[i], 0);
}
/* Every how many snapshots peer p gets one: a car hundreds of metres away is
 * out of sight, and with the hub's 32 nearest players most of a crowd's
 * links cross the map.  Our position against its newest state's. */
static int snap_every(const rc_peer *p, const rc_state *st){
    if (!p->mo.n) return 1;
    const float *q = p->mo.s[p->mo.n - 1].p;
    float dx = q[0] - st->p[0], dy = q[1] - st->p[1], d2 = dx * dx + dy * dy;
    return d2 > RC_VERYFAR_M * RC_VERYFAR_M ? 10 : d2 > RC_FAR_M * RC_FAR_M ? 4 : 1;
}
#define SNAP_MAX (RC_HDR + 6 + RC_SNAP_STATES * (RC_STATE_BYTES + RC_STATEX_BYTES) + RC_CRC)
int rc_push_state(rc_session *s, const rc_state *st, int64_t now){
    int64_t iv = (int32_t)1000000 / (int32_t)s->conf.rate_hz;
    if (s->closed || now < s->next_snap_us) return 0;
    s->next_snap_us = (now - s->next_snap_us > iv) ? now + iv : s->next_snap_us + iv;
    /* keep the last states, newest first */
    if (s->nlast && s->last[0].t_us == st->t_us && s->status == RC_ST_PAUSED){ /* paused: same state again */ }
    else {
        memmove(&s->last[1], &s->last[0], sizeof s->last[0] * (RC_SNAP_STATES - 1));
        s->last[0] = *st; if (s->nlast < RC_SNAP_STATES) s->nlast++;
    }
    /* the hub gets one too, now and then: where we are */
    int to_hub = s->conf.hub_ip && now - s->hub_snap_tx >= T_HUB_SNAP;
    if (!rc_established(s) && !to_hub) return 1;
    s->seq++;
    rc_msg m; hdr(s, &m, RC_MSG_SNAP);
    m.u.snap.status = s->status; m.u.snap.epoch = s->epoch; m.u.snap.clock = s->clock; m.u.snap.cfg_rev = s->cfg_rev;
    m.u.snap.n = (uint8_t)(s->nlast < s->conf.snap_states ? s->nlast : s->conf.snap_states);
    for (int k = 0; k < m.u.snap.n; k++) m.u.snap.s[k] = s->last[k];
    uint8_t b[RC_MAX_DGRAM]; int n = rc_encode(&m, b, sizeof b);
    if (n <= 0) return 0;
    /* the same SNAP with the states' extensions for a peer whose HELLO says
     * it reads them (when the game side filled them in), and with header
     * flags 4 and 10 for the peers far away: each made when one needs it,
     * and each far peer at its own phase, so that the far ones' snapshots
     * spread over the ticks.  bv[0]: near, extended; bv[1 + 2x + j]: far
     * (x: extended, j: 0 every 4th, 1 every 10th) */
    int ext = 0;
    for (int k = 0; k < m.u.snap.n; k++) ext |= m.u.snap.s[k].ext;
    uint8_t bv[5][SNAP_MAX]; int nv[5] = { 0, 0, 0, 0, 0 };
    for (int i = 0; i < RC_TABLE; i++){
        rc_peer *p = &s->peer[i];
        if (p->state != RC_PS_EST || !s->io.send) continue;
        int e = snap_every(p, &s->last[0]), x = ext && p->statex;
        if (e > 1 && (uint16_t)(s->seq + i) % (uint16_t)e) continue;
        const uint8_t *d = b; int dn = n;
        if (e > 1 || x){
            int v = e == 1 ? 0 : 1 + 2 * x + (e == 4 ? 0 : 1);
            if (!nv[v]){
                m.h.flags = (uint16_t)((e > 1 ? e : 0) | (x ? RC_HF_STATEX : 0));
                nv[v] = rc_encode(&m, bv[v], SNAP_MAX); m.h.flags = 0;
            }
            if (nv[v] <= 0) continue;
            d = bv[v]; dn = nv[v];
        }
        s->io.send(s->io.user, p->ip, p->port, d, dn); s->tx++; s->tx_bytes += (uint32_t)dn;
    }
    if (to_hub && s->io.send){
        s->io.send(s->io.user, s->conf.hub_ip, s->conf.hub_port, b, n); s->tx++; s->tx_bytes += (uint32_t)n;
        s->hub_snap_tx = now;
    }
    return 2;
}
void rc_shutdown(rc_session *s, uint8_t reason){
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < RC_TABLE; i++){
            rc_peer *p = &s->peer[i];
            if (p->state != RC_PS_EST && p->state != RC_PS_HEARD) continue;
            rc_msg m; hdr(s, &m, RC_MSG_BYE); m.u.bye.reason = reason; tx(s, p, &m);
        }
    if (s->conf.hub_ip){ rc_msg m; hdr(s, &m, RC_MSG_BYE); m.u.bye.reason = reason; tx_to(s, s->conf.hub_ip, s->conf.hub_port, &m); }
    s->closed = 1;
}
