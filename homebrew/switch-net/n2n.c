/* n2n.c -- see n2n.h.  A WebSocket client (RFC 6455, enough for this server) and the switch's JSON control messages. */
#include "n2n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

#define MAX_MSG      (1u << 20)   /* the longest message taken (a peer list on a full switch with LAN play open is ~420 KB) */
#define PING_MS      10000
#define CLOSE_KICKED 4001
#define CLOSE_BANNED 4003

struct n2n {
    n2n_io io; int open;
    n2n_cb cb; void *user;
    char *host, *path, *name, *id; int port, tls;
    uint8_t *rx; size_t rxn, rxcap;           /* bytes from the socket, not yet taken as frames */
    uint8_t *msg; size_t msgn, msgcap; int msg_op;   /* a message being put together from its fragments */
    int online; uint32_t vip, netmask; char token[64], ticket[64];
    int lan; uint32_t udp;                    /* the hello says LAN play is open (and which UDP ports have a socket) */
    n2n_peer *peers; int npeers, peercap;
    uint32_t ping_id, pending_id; double pending_at, last_ping;
    int removed, removed_ban; char removed_msg[256];
    int closed_sent, status;
};

static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void random_bytes(void *p, size_t n) {
    uint8_t *b = p;
#ifdef __SWITCH__
    randomGet(p, n); return;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { size_t r = fread(b, 1, n, f); fclose(f); if (r == n) return; }
    static uint64_t s; if (!s) s = (uint64_t)now_ms() * 2654435761u ^ (uint64_t)(uintptr_t)p;
    for (size_t i = 0; i < n; i++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; b[i] = (uint8_t)s; }
#endif
}

/* ---- JSON: just enough for the switch's flat control messages (it sends compact JSON; this takes any) ---- */

static const char *jws(const char *p, const char *e) { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; return p; }

/* after the value that starts at p, or NULL */
static const char *jskip(const char *p, const char *e) {
    p = jws(p, e);
    if (p >= e) return NULL;
    if (*p == '"') {
        for (p++; p < e; p++) { if (*p == '\\') p++; else if (*p == '"') return p + 1; }
        return NULL;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        for (; p < e; p++) {
            if (*p == '"') { const char *q = jskip(p, e); if (!q) return NULL; p = q - 1; }
            else if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
        }
        return NULL;
    }
    while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    return p;
}

/* the value of key in the object at o, or NULL */
static const char *jget(const char *o, const char *e, const char *key) {
    size_t kl = strlen(key);
    o = jws(o, e);
    if (o >= e || *o != '{') return NULL;
    o++;
    for (;;) {
        o = jws(o, e);
        if (o >= e || *o != '"') return NULL;
        const char *k = o + 1, *kend = jskip(o, e);
        if (!kend) return NULL;
        int match = (size_t)(kend - 1 - k) == kl && !memcmp(k, key, kl);
        o = jws(kend, e);
        if (o >= e || *o != ':') return NULL;
        o = jws(o + 1, e);
        if (match) return o;
        o = jskip(o, e);
        if (!o) return NULL;
        o = jws(o, e);
        if (o < e && *o == ',') { o++; continue; }
        return NULL;
    }
}

/* the next element of the array at a (prev: NULL for the first, else the element before), or NULL */
static const char *jnext(const char *a, const char *e, const char *prev) {
    const char *p;
    if (!prev) { a = jws(a, e); if (a >= e || *a != '[') return NULL; p = a + 1; }
    else { p = jskip(prev, e); if (!p) return NULL; p = jws(p, e); if (p < e && *p == ',') p++; }
    p = jws(p, e);
    return (p < e && *p != ']') ? p : NULL;
}

static void put_utf8(char **o, char *end, uint32_t c) {
    char t[4]; int n;
    if (c < 0x80) { t[0] = (char)c; n = 1; }
    else if (c < 0x800) { t[0] = (char)(0xC0 | c >> 6); t[1] = (char)(0x80 | (c & 63)); n = 2; }
    else if (c < 0x10000) { t[0] = (char)(0xE0 | c >> 12); t[1] = (char)(0x80 | (c >> 6 & 63)); t[2] = (char)(0x80 | (c & 63)); n = 3; }
    else { t[0] = (char)(0xF0 | c >> 18); t[1] = (char)(0x80 | (c >> 12 & 63)); t[2] = (char)(0x80 | (c >> 6 & 63)); t[3] = (char)(0x80 | (c & 63)); n = 4; }
    if (*o + n > end) return;
    memcpy(*o, t, n); *o += n;
}
static int hex4(const char *p, const char *e, uint32_t *v) {
    if (e - p < 4) return 0;
    *v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i]; *v <<= 4;
        if (c >= '0' && c <= '9') *v |= (uint32_t)(c - '0'); else if (c >= 'a' && c <= 'f') *v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') *v |= (uint32_t)(c - 'A' + 10); else return 0;
    }
    return 1;
}

/* the string at v into out (cap bytes with the NUL; cut where it does not fit); 1, or 0 if v is no string */
static int jstr(const char *v, const char *e, char *out, size_t cap) {
    if (!v || v >= e || *v != '"' || !cap) { if (cap) out[0] = 0; return 0; }
    char *o = out, *end = out + cap - 1;
    for (v++; v < e && *v != '"'; v++) {
        if (*v != '\\') { if (o < end) *o++ = *v; continue; }
        if (++v >= e) break;
        switch (*v) {
        case 'n': put_utf8(&o, end, '\n'); break;
        case 't': put_utf8(&o, end, '\t'); break;
        case 'r': put_utf8(&o, end, '\r'); break;
        case 'b': put_utf8(&o, end, '\b'); break;
        case 'f': put_utf8(&o, end, '\f'); break;
        case 'u': {
            uint32_t c, d;
            if (!hex4(v + 1, e, &c)) { *o = 0; return 0; }
            v += 4;
            if (c >= 0xD800 && c < 0xDC00 && e - v > 6 && v[1] == '\\' && v[2] == 'u' && hex4(v + 3, e, &d) && d >= 0xDC00 && d < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); v += 6;
            }
            put_utf8(&o, end, c); break;
        }
        default: put_utf8(&o, end, (unsigned char)*v);   /* \" \\ \/ */
        }
    }
    *o = 0;
    return 1;
}
static int jnum(const char *v, const char *e, double *out) {
    if (!v || v >= e) return 0;
    char *end; double d = strtod(v, &end);        /* the message is NUL-terminated (put_message) */
    if (end == v || end > e) return 0;
    *out = d; return 1;
}
static int jtrue(const char *v, const char *e) { return v && e - v >= 4 && !memcmp(v, "true", 4); }

/* the string for a hello: JSON-escaped, cut to max bytes where a character starts */
static size_t jesc(char *o, size_t cap, const char *s, size_t max) {
    size_t n = 0, i = 0;
    for (; s[i] && i < max; i++) {
        unsigned char c = (unsigned char)s[i];
        if (n + 8 > cap) break;
        if (c == '"' || c == '\\') { o[n++] = '\\'; o[n++] = (char)c; }
        else if (c < 0x20) n += (size_t)snprintf(o + n, 8, "\\u%04x", c);
        else o[n++] = (char)c;
    }
    if (s[i] && ((unsigned char)s[i] & 0xC0) == 0x80) {       /* cut inside a character: drop what of it is there */
        while (n && ((unsigned char)o[n - 1] & 0xC0) == 0x80) n--;
        if (n) n--;
    }
    o[n] = 0;
    return n;
}

/* ---- the object ---- */

static char *n2n_dup(const char *s) { size_t n = strlen(s) + 1; char *p = malloc(n); if (p) memcpy(p, s, n); return p; }

n2n *n2n_new(const char *url, const char *name, const char *id, n2n_cb cb, void *user) {
    struct { const char *p; int tls, port; } sch[] = { {"wss://", 1, 443}, {"https://", 1, 443}, {"ws://", 0, 80}, {"http://", 0, 80} };
    int tls = -1, port = 0; const char *rest = NULL;
    for (size_t i = 0; i < 4; i++) if (!strncmp(url, sch[i].p, strlen(sch[i].p))) { tls = sch[i].tls; port = sch[i].port; rest = url + strlen(sch[i].p); }
    if (!rest || !*rest) return NULL;
    const char *slash = strchr(rest, '/'), *hend = slash ? slash : rest + strlen(rest);
    const char *h0 = rest, *h1 = hend, *colon = NULL;
    if (*rest == '[') {                                   /* [::1]:8080 */
        const char *rb = memchr(rest, ']', (size_t)(hend - rest));
        if (!rb) return NULL;
        h0 = rest + 1; h1 = rb;
        if (rb + 1 < hend) { if (rb[1] != ':') return NULL; colon = rb + 1; }
    } else {
        for (const char *p = rest; p < hend; p++) if (*p == ':') { colon = p; h1 = p; }
    }
    n2n *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->host = malloc((size_t)(h1 - h0) + 1);
    if (!c->host) { free(c); return NULL; }
    memcpy(c->host, h0, (size_t)(h1 - h0)); c->host[h1 - h0] = 0;
    if (colon) port = atoi(colon + 1);
    c->port = port; c->tls = tls;
    const char *path = slash && slash[1] && slash[1] != '?' ? slash : "/lan";
    c->path = n2n_dup(path);
    c->name = n2n_dup(name ? name : "player"); c->id = id && *id ? n2n_dup(id) : NULL;
    c->cb = cb; c->user = user;
    if (!c->path || !c->name || port <= 0 || port > 65535 || !*c->host) { n2n_free(c); return NULL; }
    return c;
}

void n2n_close(n2n *c) {
    if (c->open) {
        /* a close frame with 1000, so the switch frees our address at once */
        uint8_t body[8] = { 0x03, 0xE8, 'c', 'l', 'o', 's', 'e', 'd' }, f[2 + 4 + 8];
        uint8_t mask[4]; random_bytes(mask, 4);
        f[0] = 0x88; f[1] = 0x80 | 8; memcpy(f + 2, mask, 4);
        for (int i = 0; i < 8; i++) f[6 + i] = body[i] ^ mask[i & 3];
        c->io.send(c->io.ctx, f, sizeof f);
        c->io.close(c->io.ctx);
        c->open = 0;
    }
    c->online = 0;
}

void n2n_free(n2n *c) {
    if (!c) return;
    n2n_close(c);
    free(c->host); free(c->path); free(c->name); free(c->id); free(c->rx); free(c->msg); free(c->peers); free(c);
}

int n2n_online(const n2n *c) { return c->online; }
uint32_t n2n_vip(const n2n *c) { return c->vip; }
const n2n_peer *n2n_peers(const n2n *c, int *count) { if (count) *count = c->npeers; return c->peers; }
int n2n_http_status(const n2n *c) { return c->status; }

/* ---- sending ---- */

static int ws_send(n2n *c, int op, const void *data, size_t n) {
    if (!c->open) return N2N_ERR_IO;
    size_t hl = 2 + (n > 65535 ? 8 : n > 125 ? 2 : 0) + 4;
    uint8_t *f = malloc(hl + n);
    if (!f) return N2N_ERR_IO;
    size_t i = 0;
    f[i++] = (uint8_t)(0x80 | op);
    if (n > 65535) { f[i++] = 0x80 | 127; for (int s = 56; s >= 0; s -= 8) f[i++] = (uint8_t)((uint64_t)n >> s); }
    else if (n > 125) { f[i++] = 0x80 | 126; f[i++] = (uint8_t)(n >> 8); f[i++] = (uint8_t)n; }
    else f[i++] = (uint8_t)(0x80 | n);
    uint8_t mask[4]; random_bytes(mask, 4);
    memcpy(f + i, mask, 4); i += 4;
    for (size_t k = 0; k < n; k++) f[i + k] = ((const uint8_t *)data)[k] ^ mask[k & 3];
    int r = c->io.send(c->io.ctx, f, i + n);
    free(f);
    if (r) { c->open = 0; return N2N_ERR_IO; }
    return 0;
}

int n2n_send_frame(n2n *c, const void *frame, size_t len) {
    if (len < 16 || len > 8192) return N2N_ERR_IO;
    return ws_send(c, 2, frame, len);
}

int n2n_send_udp(n2n *c, uint32_t dst, uint16_t sport, uint16_t dport, const void *data, size_t len) {
    if (len > N2N_MAX_PAYLOAD) return N2N_ERR_IO;
    uint8_t f[16 + N2N_MAX_PAYLOAD] = { N2N_KIND_UDP };          /* the switch writes the source address */
    f[8] = (uint8_t)(dst >> 24); f[9] = (uint8_t)(dst >> 16); f[10] = (uint8_t)(dst >> 8); f[11] = (uint8_t)dst;
    f[12] = (uint8_t)(sport >> 8); f[13] = (uint8_t)sport; f[14] = (uint8_t)(dport >> 8); f[15] = (uint8_t)dport;
    if (len) memcpy(f + 16, data, len);
    return ws_send(c, 2, f, 16 + len);
}

static int send_text(n2n *c, const char *s) { return ws_send(c, 1, s, strlen(s)); }

int n2n_ping(n2n *c) {
    char b[64];
    c->pending_id = ++c->ping_id; c->pending_at = now_ms();
    snprintf(b, sizeof b, "{\"t\":\"ping\",\"id\":%u}", c->pending_id);
    return send_text(c, b);
}

/* ---- connecting ---- */

static int rbuf_add(n2n *c, const void *p, size_t n) {
    if (c->rxn + n > c->rxcap) {
        size_t cap = c->rxcap ? c->rxcap : 4096;
        while (cap < c->rxn + n) cap *= 2;
        uint8_t *b = realloc(c->rx, cap);
        if (!b) return -1;
        c->rx = b; c->rxcap = cap;
    }
    memcpy(c->rx + c->rxn, p, n); c->rxn += n;
    return 0;
}

static void base64(const uint8_t *s, size_t n, char *o) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, k = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = (uint32_t)s[i] << 16 | (uint32_t)s[i + 1] << 8 | s[i + 2];
        o[k++] = T[v >> 18]; o[k++] = T[v >> 12 & 63]; o[k++] = T[v >> 6 & 63]; o[k++] = T[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)s[i] << 16 | (i + 1 < n ? (uint32_t)s[i + 1] << 8 : 0);
        o[k++] = T[v >> 18]; o[k++] = T[v >> 12 & 63]; o[k++] = i + 1 < n ? T[v >> 6 & 63] : '='; o[k++] = '=';
    }
    o[k] = 0;
}

int n2n_connect(n2n *c, int timeout_ms) {
    n2n_close(c);
    c->rxn = 0; c->msgn = 0; c->msg_op = 0; c->closed_sent = 0; c->removed = 0; c->status = 0;
    c->npeers = 0; c->pending_id = 0;
    int r;
    if (c->tls) {
#ifdef N2N_TLS
        r = n2n_tls_connect(c->host, c->port, timeout_ms, &c->io);
#else
        return N2N_ERR_TLS;     /* built without TLS: use ws:// (a local server) or build with -DN2N_TLS */
#endif
    } else r = n2n_tcp_connect(c->host, c->port, timeout_ms, &c->io);
    if (r) return r < 0 ? r : N2N_ERR_CONNECT;
    c->open = 1;

    int resume = (c->vip && c->token[0]) || c->ticket[0];
    uint8_t key[16]; char key64[32], req[1024];
    random_bytes(key, 16); base64(key, 16, key64);
    int hostv6 = strchr(c->host, ':') != NULL;
    int n = snprintf(req, sizeof req,
        "GET %s%squeue%s HTTP/1.1\r\nHost: %s%s%s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: nfsu2-switch-net\r\n\r\n",
        c->path, strchr(c->path, '?') ? "&" : "?", resume ? "&resume" : "", hostv6 ? "[" : "", c->host, hostv6 ? "]" : "", c->port, key64);
    if (n <= 0 || (size_t)n >= sizeof req || c->io.send(c->io.ctx, req, (size_t)n)) { n2n_close(c); return N2N_ERR_IO; }

    /* the response head: up to the empty line; what follows it is already WebSocket frames */
    double t0 = now_ms(); uint8_t buf[1024]; size_t have = 0; char *eoh = NULL;
    char head[4096];
    while (!eoh) {
        int left = timeout_ms - (int)(now_ms() - t0);
        if (left <= 0 || have >= sizeof head - 1) { n2n_close(c); return N2N_ERR_HANDSHAKE; }
        int g = c->io.recv(c->io.ctx, buf, sizeof buf, left);
        if (g < 0) { n2n_close(c); return N2N_ERR_HANDSHAKE; }
        if (g == 0) continue;
        size_t take = (size_t)g < sizeof head - 1 - have ? (size_t)g : sizeof head - 1 - have;
        memcpy(head + have, buf, take); have += take; head[have] = 0;
        eoh = strstr(head, "\r\n\r\n");
        if (!eoh && take < (size_t)g) { n2n_close(c); return N2N_ERR_HANDSHAKE; }
    }
    { const char *sp = strchr(head, ' '); c->status = sp ? atoi(sp + 1) : 0; }
    if (c->status != 101) { n2n_close(c); return N2N_ERR_REFUSED; }
    size_t hl = (size_t)(eoh - head) + 4;
    if (have > hl && rbuf_add(c, head + hl, have - hl)) { n2n_close(c); return N2N_ERR_IO; }

    char hello[1024], nm[300], idb[160] = "", rs[256] = "";
    jesc(nm, sizeof nm, c->name, N2N_NAME_BYTES);
    if (c->id) { char e[140]; jesc(e, sizeof e, c->id, 64); snprintf(idb, sizeof idb, ",\"id\":\"%s\"", e); }
    if (c->vip && c->token[0]) snprintf(rs, sizeof rs, ",\"vip\":\"%u.%u.%u.%u\",\"token\":\"%s\"", c->vip >> 24, c->vip >> 16 & 255, c->vip >> 8 & 255, c->vip & 255, c->token);
    if (c->ticket[0]) snprintf(rs + strlen(rs), sizeof rs - strlen(rs), ",\"ticket\":\"%s\"", c->ticket);
    char ub[40] = ""; if (c->lan && c->udp) snprintf(ub, sizeof ub, ",\"udp\":%u", c->udp);
    snprintf(hello, sizeof hello, "{\"t\":\"hello\",\"name\":\"%s\",\"lan\":%d%s,\"bundle\":1%s%s}", nm, c->lan ? 1 : 0, ub, idb, rs);
    if (send_text(c, hello)) { n2n_close(c); return N2N_ERR_IO; }
    c->last_ping = now_ms();
    return 0;
}

/* ---- receiving ---- */

static void emit(n2n *c, n2n_event *ev) { if (c->cb) c->cb(c->user, ev); }

static void peers_changed(n2n *c) { n2n_event ev = { .type = N2N_EV_PEERS }; emit(c, &ev); }

static int peer_find(n2n *c, uint32_t vip) { for (int i = 0; i < c->npeers; i++) if (c->peers[i].vip == vip) return i; return -1; }

static void peer_set(n2n *c, const char *o, const char *e) {
    double vip; char name[N2N_NAME_BYTES + 1];
    if (!jnum(jget(o, e, "vip"), e, &vip) || vip < 0 || vip > 4294967295.0) return;
    jstr(jget(o, e, "name"), e, name, sizeof name);
    int i = peer_find(c, (uint32_t)vip);
    if (i < 0) {
        if (c->npeers == c->peercap) {
            int cap = c->peercap ? c->peercap * 2 : 16;
            n2n_peer *p = realloc(c->peers, (size_t)cap * sizeof *p);
            if (!p) return;
            c->peers = p; c->peercap = cap;
        }
        i = c->npeers++;
    }
    c->peers[i].vip = (uint32_t)vip;
    memcpy(c->peers[i].name, name, sizeof name);
    c->peers[i].server = jtrue(jget(o, e, "server"), e);
}

static void text_message(n2n *c, const char *m, size_t len) {
    const char *e = m + len;
    char t[16];
    if (!jstr(jget(m, e, "t"), e, t, sizeof t)) return;
    if (!strcmp(t, "welcome")) {
        double vip;
        if (!jnum(jget(m, e, "vip"), e, &vip)) return;
        c->vip = (uint32_t)vip; c->online = 1; c->ticket[0] = 0;
        { char mk[24]; unsigned a, b2, c2, d; c->netmask = 0xffffff00u;
          if (jstr(jget(m, e, "mask"), e, mk, sizeof mk) && sscanf(mk, "%u.%u.%u.%u", &a, &b2, &c2, &d) == 4) c->netmask = (a << 24) | (b2 << 16) | (c2 << 8) | d; }
        jstr(jget(m, e, "token"), e, c->token, sizeof c->token);
        n2n_event ev = { .type = N2N_EV_WELCOME, .vip = c->vip, .token = c->token };
        emit(c, &ev);
    } else if (!strcmp(t, "queue")) {
        double pos, of;
        if (!jnum(jget(m, e, "pos"), e, &pos) || !jnum(jget(m, e, "of"), e, &of)) return;
        jstr(jget(m, e, "ticket"), e, c->ticket, sizeof c->ticket);
        n2n_event ev = { .type = N2N_EV_QUEUE, .pos = (int)pos, .of = (int)of };
        emit(c, &ev);
    } else if (!strcmp(t, "peers")) {
        const char *a = jget(m, e, "peers");
        if (!a) return;
        c->npeers = 0;
        for (const char *p = jnext(a, e, NULL); p; p = jnext(a, e, p)) peer_set(c, p, e);
        peers_changed(c);
    } else if (!strcmp(t, "delta")) {
        const char *g = jget(m, e, "gone"), *a = jget(m, e, "peers");
        if (!g || !a) return;
        for (const char *p = jnext(g, e, NULL); p; p = jnext(g, e, p)) {      /* gone first: one may be back in peers */
            double v; int i;
            if (jnum(p, e, &v) && (i = peer_find(c, (uint32_t)v)) >= 0) c->peers[i] = c->peers[--c->npeers];
        }
        for (const char *p = jnext(a, e, NULL); p; p = jnext(a, e, p)) peer_set(c, p, e);
        peers_changed(c);
    } else if (!strcmp(t, "pong")) {
        double id;
        if (!jnum(jget(m, e, "id"), e, &id) || c->pending_id == 0 || (uint32_t)id != c->pending_id) return;
        c->pending_id = 0;
        n2n_event ev = { .type = N2N_EV_PONG, .rtt_ms = now_ms() - c->pending_at };
        emit(c, &ev);
    } else if (!strcmp(t, "kick") || !strcmp(t, "ban")) {
        c->removed = 1; c->removed_ban = t[0] == 'b';
        jstr(jget(m, e, "msg"), e, c->removed_msg, sizeof c->removed_msg);
        n2n_event ev = { .type = N2N_EV_REMOVED, .ban = c->removed_ban, .msg = c->removed_msg };
        emit(c, &ev);
    }
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void frame_message(n2n *c, const uint8_t *f, size_t n) {
    if (n < 16 || f[0] == 32) return;
    n2n_event ev = { .type = N2N_EV_FRAME, .kind = f[0], .src = be32(f + 4), .dst = be32(f + 8),
                     .sport = (uint16_t)(f[12] << 8 | f[13]), .dport = (uint16_t)(f[14] << 8 | f[15]), .data = f + 16, .len = n - 16 };
    emit(c, &ev);
}

/* a binary message: one frame, or kind 32, a bundle: [1..3] 0, then entries of [0] k [1] 0 [2..3] frame length,
 * k x u32 destinations (none from the switch), the frame */
static void binary_message(n2n *c, const uint8_t *d, size_t n) {
    if (n < 1) return;
    if (d[0] != 32) { frame_message(c, d, n); return; }
    size_t at = 4;
    while (at + 4 <= n) {
        size_t k = d[at], len = (size_t)(d[at + 2] << 8 | d[at + 3]), f = at + 4 + 4 * k;
        if (len < 16 || f + len > n) return;
        frame_message(c, d + f, len);
        at = f + len;
    }
}

static void put_message(n2n *c, int op, const uint8_t *p, size_t n, int fin) {
    if (op) { c->msg_op = op; c->msgn = 0; }
    if (c->msgn + n > MAX_MSG) { c->msg_op = 0; c->msgn = 0; return; }
    if (c->msgn + n + 1 > c->msgcap) {
        size_t cap = c->msgcap ? c->msgcap : 2048;
        while (cap < c->msgn + n + 1) cap *= 2;
        uint8_t *b = realloc(c->msg, cap);
        if (!b) { c->msg_op = 0; c->msgn = 0; return; }
        c->msg = b; c->msgcap = cap;
    }
    if (n) memcpy(c->msg + c->msgn, p, n);
    c->msgn += n; c->msg[c->msgn] = 0;
    if (!fin) return;
    int o = c->msg_op; c->msg_op = 0;
    if (o == 1) text_message(c, (const char *)c->msg, c->msgn);
    else if (o == 2) binary_message(c, c->msg, c->msgn);
}

/* takes the whole frames in the buffer; -1 once the connection ended */
static int parse(n2n *c) {
    size_t at = 0;
    while (c->open) {
        const uint8_t *p = c->rx + at; size_t avail = c->rxn - at;
        if (avail < 2) break;
        int fin = p[0] >> 7, op = p[0] & 15, masked = p[1] >> 7; uint64_t len = p[1] & 127; size_t hl = 2;
        if (len == 126) { if (avail < 4) break; len = (uint64_t)(p[2] << 8 | p[3]); hl = 4; }
        else if (len == 127) {
            if (avail < 10) break;
            len = 0; for (int i = 0; i < 8; i++) len = len << 8 | p[2 + i];
            hl = 10;
        }
        if (masked) hl += 4;
        if (len > MAX_MSG) { n2n_close(c); return -1; }
        if (avail < hl + len) break;
        uint8_t *body = c->rx + at + hl;
        if (masked) for (size_t i = 0; i < len; i++) body[i] ^= p[hl - 4 + (i & 3)];
        at += hl + (size_t)len;
        if (op == 8) {
            int code = len >= 2 ? body[0] << 8 | body[1] : 0;
            n2n_close(c);
            n2n_event ev = { .type = N2N_EV_CLOSED, .code = code ? code : (c->removed ? (c->removed_ban ? CLOSE_BANNED : CLOSE_KICKED) : 0) };
            c->closed_sent = 1; emit(c, &ev);
            return -1;
        }
        if (op == 9) ws_send(c, 10, body, (size_t)len);                 /* ping: pong with the same bytes */
        else if (op == 10) { }
        else if (op == 1 || op == 2) put_message(c, op, body, (size_t)len, fin);
        else if (op == 0) put_message(c, 0, body, (size_t)len, fin);
    }
    if (at) { memmove(c->rx, c->rx + at, c->rxn - at); c->rxn -= at; }
    return c->open ? 0 : -1;
}

int n2n_poll(n2n *c, int timeout_ms) {
    if (!c->open) return -1;
    if (c->rxn && parse(c) < 0) return -1;
    uint8_t buf[4096];
    int g = c->io.recv(c->io.ctx, buf, sizeof buf, timeout_ms);
    if (g < 0) {
        n2n_close(c);
        if (!c->closed_sent) { n2n_event ev = { .type = N2N_EV_CLOSED, .code = 0 }; c->closed_sent = 1; emit(c, &ev); }
        return -1;
    }
    if (g > 0) {
        if (rbuf_add(c, buf, (size_t)g)) { n2n_close(c); return -1; }
        if (parse(c) < 0) return -1;
    }
    if (c->online && now_ms() - c->last_ping >= PING_MS) { c->last_ping = now_ms(); n2n_ping(c); }
    return c->open ? 0 : -1;
}

void n2n_set_lan(n2n *c, int lan, uint32_t udp_ports) { c->lan = lan; c->udp = udp_ports; }
void n2n_set_resume(n2n *c, uint32_t vip, const char *token) { c->vip = vip; snprintf(c->token, sizeof c->token, "%s", token ? token : ""); }
uint32_t n2n_netmask(const n2n *c) { return c->netmask ? c->netmask : 0xffffff00u; }
int n2n_send_json(n2n *c, const char *json) { return send_text(c, json); }
