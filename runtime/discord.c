/* discord.c -- Discord Rich Presence for the native port (no SDK, no library).
 *
 * The Discord desktop app listens on a local Unix socket (discord-ipc-0..9
 * under $XDG_RUNTIME_DIR, $TMPDIR, $TMP, $TEMP or /tmp).  A frame is an
 * 8-byte little-endian header {uint32 opcode, uint32 length} followed by a
 * UTF-8 JSON payload.  Opcodes: 0 HANDSHAKE, 1 FRAME, 2 CLOSE, 3 PING, 4 PONG.
 * We send HANDSHAKE {"v":1,"client_id":...}, wait for the READY dispatch, then
 * SET_ACTIVITY commands (with our pid and a nonce).  Nothing goes over the
 * network from here: the socket is local and Discord does the rest.
 *
 * Threads.  The game thread only samples guest memory (discord_on_present,
 * called from IDirect3DDevice9::Present, at most DS_SAMPLE_HZ times a second)
 * and hands a finished text snapshot to the IPC thread under a mutex it only
 * try-locks; it never touches the socket and never waits.  The IPC thread
 * owns the socket: it connects, reconnects with backoff (Discord starting,
 * quitting or restarting mid-game), answers PING, rate-limits SET_ACTIVITY
 * (Discord accepts about 5 updates per 20 s; we send at most DS_RATE_N per
 * DS_RATE_WIN s, coalescing to the latest state, and only on change), and
 * clears the activity on a clean quit.  When Discord is not running the cost
 * is one connect() scan every DS_RETRY_MAX s, in a thread that otherwise
 * sleeps in poll().
 *
 * Options (environment; the app's settings.ini maps lower-case keys onto
 * NFSU2_<KEY>, e.g. "discord = off" -> NFSU2_DISCORD=off):
 *   NFSU2_DISCORD=1|0          on by default; 0/off/no/false: no thread, no
 *                              socket, nothing (tools/autorun.sh and
 *                              tests/run_suite.sh default it to 0)
 *   NFSU2_DISCORD_APP_ID=<id>  Discord application (client) id; default is
 *                              Discord's own "Need for Speed: Underground 2"
 *                              entry, 1442720706248052796
 *   NFSU2_DISCORD_IPC=<path>   connect to exactly this socket and nothing else
 *                              (tests point it at tools/mock_discord.py)
 *   NFSU2_DISCORD_DETAIL=full|minimal
 *                              minimal: only "Playing" plus the elapsed time;
 *                              no race, car, track or profile details
 *   NFSU2_DISCORD_LARGE_IMAGE=<asset key or https URL>
 *                              optional large image (an asset key only works
 *                              with your own application id); unset: none,
 *                              Discord shows the application's icon if any
 *   NFSU2_DISCORD_LOG=0..3     0: connect/disconnect and refusals only;
 *                              1: also every presence shown, frame pacing every
 *                              5 s and the sampler's cost at exit; 2: also every
 *                              frame (never the READY payload, which holds the
 *                              user's Discord name); 3: also the raw game state
 *                              every 2 s
 * A test or diagnostic run without NFSU2_DISCORD_IPC never looks for the
 * Discord app: a script or frame limit (NFSU2_INPUT_SCRIPT, NFSU2_QUIT_FRAMES,
 * QUITFRAMES), headless (no WINDOW), a background test window (NOFOCUS) or a
 * time limit (ALARM/NFSU2_TIMEOUT not 0; boot.c's default is 20 s).
 *
 * Game state: see ds_sample() below for the guest addresses (v1.2 NA) and
 * the situation -> text table.
 */
#include "cpu.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DS_DEFAULT_APP_ID "1442720706248052796"  /* Discord: "Need for Speed: Underground 2" */
#define DS_TXT        512      /* bytes per text field (UTF-8, <= 128 characters) */
#define DS_MAXCHARS   128      /* Discord's limit for details/state */
/* Timings; tests/discord_unit.c compiles this file with shorter ones. */
#ifndef DS_SAMPLE_HZ
#define DS_SAMPLE_HZ  4        /* game-state samples per second (game thread) */
#endif
#ifndef DS_SETTLE_MS
#define DS_SETTLE_MS  1000     /* a new first line must hold this long before it is shown */
#endif
#ifndef DS_RATE_N
#define DS_RATE_N     4        /* at most this many SET_ACTIVITY ... */
#endif
#ifndef DS_RATE_WIN
#define DS_RATE_WIN   20.0     /* ... per this many seconds (Discord: 5 per 20 s) */
#endif
#ifndef DS_MINOR_GAP
#define DS_MINOR_GAP  15.0     /* a change of the second line only (lap, points) at most this often */
#endif
#ifndef DS_READY_TMO
#define DS_READY_TMO  5.0      /* handshake -> READY */
#endif
#ifndef DS_RETRY_MIN
#define DS_RETRY_MIN  2.0      /* reconnect backoff, doubling up to DS_RETRY_MAX */
#endif
#ifndef DS_RETRY_MAX
#define DS_RETRY_MAX  20.0
#endif
#ifndef DS_QUIT_WAIT_MS
#define DS_QUIT_WAIT_MS 600    /* discord_shutdown waits at most this long */
#endif
#ifndef DS_UNPARK_S
#define DS_UNPARK_S   2.0      /* after a background park: wait this long for a fresh sample */
#endif
/* The app parked the game in the background (lifecycle.c: told inactive,
 * frozen in GetMessage, so no Present and no samples).  Not during a LAN
 * session: there an inactive game keeps running and the presence follows it
 * (lifecycle_game_parked).  tests/discord_unit.c supplies its own. */
#ifndef DS_PARKED
int lifecycle_game_parked(void);
#define DS_PARKED() lifecycle_game_parked()
#endif
#define DS_MAXFRAME   65536    /* larger frames are treated as garbage */

/* ---- options --------------------------------------------------------- */
static int  g_on = -1;                 /* -1: not initialised yet */
static int  g_minimal;
static int  g_log;
static char g_app_id[32];
static char g_ipc_path[PATH_MAX];      /* NFSU2_DISCORD_IPC override */
static char g_large_image[300];

static void dlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void dlog(const char *fmt, ...){
    char b[700]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fprintf(stderr, "[discord] %s\n", b);
}
static double mono_s(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static int env_bool(const char *v, int def){
    if (!v || !*v) return def;
    if (!strcasecmp(v, "0") || !strcasecmp(v, "off") || !strcasecmp(v, "no") ||
        !strcasecmp(v, "false") || !strcasecmp(v, "disabled")) return 0;
    if (!strcasecmp(v, "1") || !strcasecmp(v, "on") || !strcasecmp(v, "yes") ||
        !strcasecmp(v, "true") || !strcasecmp(v, "enabled")) return 1;
    return def;
}

/* ---- presence snapshot (game thread -> IPC thread) ------------------- */
/* What the player is doing; a change of phase (or of the first line) is shown
 * at once, within the rate limit; a change of the second line alone (lap,
 * position, points) at most every DS_MINOR_GAP s. */
enum { PH_NONE, PH_BOOT, PH_TITLE, PH_MENUS, PH_CUSTOM, PH_CARLOT, PH_RESULTS, PH_LOADING, PH_MOVIE,
       PH_GRID, PH_RACE, PH_PAUSED, PH_FINISHED, PH_ROAM, PH_FREERUN, PH_LAN };
typedef struct {
    int     active;                    /* 0: no activity (cleared) */
    int     phase;                     /* PH_* */
    char    details[DS_TXT];           /* first line, UTF-8 */
    char    state[DS_TXT];             /* second line, UTF-8, may be empty */
    int64_t start;                     /* elapsed-time origin, Unix seconds; 0: none */
} Presence;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static Presence  g_pub;                /* guarded by g_mu */
static unsigned  g_pub_seq;            /* guarded by g_mu */
static int       g_wake[2] = { -1, -1 };   /* self-pipe: game thread -> IPC thread */
static pthread_t g_thr;
static _Atomic int g_started, g_stop, g_done;
static pthread_mutex_t g_done_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_done_cv = PTHREAD_COND_INITIALIZER;

static void wake_ipc(void){
    if (g_wake[1] >= 0){ char b = 1; ssize_t r = write(g_wake[1], &b, 1); (void)r; }  /* non-blocking */
}
/* While the game is parked in the background a race or free roam shows
 * "Paused", as the pause menu does (the timer stays as it is and the race
 * clock re-anchors it after the resume). */
static void pres_parked(Presence *p){
    if (p->active && (p->phase == PH_GRID || p->phase == PH_RACE || p->phase == PH_ROAM || p->phase == PH_FREERUN)){
        p->phase = PH_PAUSED; snprintf(p->state, sizeof p->state, "Paused"); }
}
static int pres_same(const Presence *a, const Presence *b){
    return a->active == b->active && a->start == b->start && a->phase == b->phase &&
           !strcmp(a->details, b->details) && !strcmp(a->state, b->state);
}
/* Same situation, at most the second line differs. */
static int pres_same_major(const Presence *a, const Presence *b){
    return a->active == b->active && a->start == b->start && a->phase == b->phase && !strcmp(a->details, b->details);
}

/* ---- text helpers ------------------------------------------------------ */
/* Guest strings are 8-bit (Windows-1252); Discord wants UTF-8. */
static const uint16_t k_cp1252[32] = {
    0x20AC,0xFFFD,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,0x02C6,0x2030,0x0160,0x2039,0x0152,0xFFFD,0x017D,0xFFFD,
    0xFFFD,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x02DC,0x2122,0x0161,0x203A,0x0153,0xFFFD,0x017E,0x0178 };
static size_t put_utf8(char *d, size_t room, unsigned cp){
    char t[4]; size_t n;
    if (cp < 0x80){ t[0] = (char)cp; n = 1; }
    else if (cp < 0x800){ t[0] = (char)(0xC0 | cp >> 6); t[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else { t[0] = (char)(0xE0 | cp >> 12); t[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    if (n >= room) return 0;
    memcpy(d, t, n); return n;
}
/* Copy a guest (cp1252) string of at most max bytes into dst as UTF-8.
 * Control characters become spaces; runs of spaces collapse; trimmed. */
static void guest_to_utf8(char *dst, size_t dsz, const uint8_t *s, size_t max){
    size_t o = 0; int sp = 1;
    for (size_t i = 0; i < max && s[i] && o + 1 < dsz; i++){
        unsigned c = s[i];
        if (c < 0x20 || c == 0x7F) c = ' ';
        if (c == ' '){ if (sp) continue; sp = 1; }
        else sp = 0;
        unsigned cp = c >= 0x80 && c < 0xA0 ? k_cp1252[c - 0x80] : c;
        size_t n = put_utf8(dst + o, dsz - o, cp);
        if (!n) break;
        o += n;
    }
    while (o && dst[o-1] == ' ') o--;
    dst[o] = 0;
}
/* Truncate UTF-8 text to at most maxch characters (adds an ellipsis), never
 * splitting a sequence; invalid bytes become U+FFFD. */
static void utf8_clamp(char *s, size_t ssz, int maxch){
    char out[DS_TXT]; size_t o = 0; int ch = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p){
        unsigned cp; int n;
        if (*p < 0x80){ cp = *p; n = 1; }
        else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80){ cp = (*p & 0x1Fu) << 6 | (p[1] & 0x3Fu); n = 2; if (cp < 0x80) cp = 0xFFFD; }
        else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80){
            cp = (*p & 0x0Fu) << 12 | (p[1] & 0x3Fu) << 6 | (p[2] & 0x3Fu); n = 3;
            if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD; }
        else if ((*p & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80){
            /* outside the BMP (not produced by the game): kept when valid
             * (U+10000..U+10FFFF, no overlong form); Discord counts it as two
             * characters (UTF-16), so do we */
            cp = (*p & 0x07u) << 18 | (p[1] & 0x3Fu) << 12 | (p[2] & 0x3Fu) << 6 | (p[3] & 0x3Fu); n = 4;
            if (cp >= 0x10000 && cp <= 0x10FFFF && o + 4 < sizeof out && ch + 2 <= maxch){
                memcpy(out + o, p, 4); o += 4; ch += 2; p += 4; continue; }
            cp = 0xFFFD; }
        else { cp = 0xFFFD; n = 1; }
        if (ch == maxch){
            /* replace the last character with an ellipsis */
            while (o && ((unsigned char)out[o-1] & 0xC0) == 0x80) o--;
            if (o) o--;
            o += put_utf8(out + o, sizeof out - o, 0x2026);
            break;
        }
        size_t w = put_utf8(out + o, sizeof out - o, cp);
        if (!w) break;
        o += w; ch++; p += n;
    }
    out[o] = 0;
    snprintf(s, ssz, "%s", out);
}
/* Append a JSON string literal (with quotes) for UTF-8 text s. */
static size_t json_str(char *d, size_t room, const char *s){
    size_t o = 0;
    #define PUT(c) do{ if (o + 1 >= room) goto full; d[o++] = (c); }while(0)
    PUT('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++){
        unsigned c = *p;
        if (c == '"' || c == '\\'){ PUT('\\'); PUT((char)c); }
        else if (c < 0x20){
            static const char hx[] = "0123456789abcdef";
            PUT('\\'); PUT('u'); PUT('0'); PUT('0'); PUT(hx[c >> 4]); PUT(hx[c & 15]); }
        else PUT((char)c);
    }
    PUT('"');
    d[o] = 0;
    return o;
full:
    d[0] = 0; return 0;
    #undef PUT
}

/* ======================================================================
 * IPC thread
 * ====================================================================== */
enum { OP_HANDSHAKE = 0, OP_FRAME = 1, OP_CLOSE = 2, OP_PING = 3, OP_PONG = 4 };
typedef struct {
    int      fd;
    int      ready;                    /* READY received */
    double   hs_deadline;
    uint8_t  rbuf[DS_MAXFRAME + 8];
    size_t   rlen;
    unsigned nonce;
    char     path[PATH_MAX];
} Conn;
static Conn g_c = { .fd = -1 };

static int send_all(int fd, const uint8_t *b, size_t n){
    double until = mono_s() + 1.0;
    while (n){
        ssize_t w = send(fd, b, n, 0);
        if (w > 0){ b += w; n -= (size_t)w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)){
            double left = until - mono_s();
            if (left <= 0) return -1;              /* peer does not read: give up */
            struct pollfd p = { fd, POLLOUT, 0 };
            poll(&p, 1, (int)(left * 1000) + 1);
            continue;
        }
        return -1;
    }
    return 0;
}
static int send_frame(Conn *c, uint32_t op, const char *json){
    size_t len = strlen(json);
    uint8_t buf[8 + 8192];
    if (len > sizeof buf - 8) return -1;
    for (int i = 0; i < 4; i++){ buf[i] = (uint8_t)(op >> (8 * i)); buf[4 + i] = (uint8_t)(len >> (8 * i)); }
    memcpy(buf + 8, json, len);
    if (g_log >= 2) dlog("-> op %u %s", op, json);
    return send_all(c->fd, buf, 8 + len);
}
static void conn_close(Conn *c, const char *why){
    if (c->fd >= 0){
        close(c->fd);
        if (c->ready || g_log) dlog("disconnected (%s)", why);
    }
    c->fd = -1; c->ready = 0; c->rlen = 0;
}

/* Candidate socket paths, in the order the official SDKs try them. */
#define DS_SUN 104                      /* sizeof sockaddr_un.sun_path on macOS */
static int ipc_candidates(char (*out)[DS_SUN], int max){
    int n = 0;
    if (g_ipc_path[0]){ snprintf(out[n++], DS_SUN, "%s", g_ipc_path); return n; }
    const char *bases[8]; int nb = 0;
    const char *ev[] = { "XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP" };
    for (int i = 0; i < 4; i++){ const char *v = getenv(ev[i]); if (v && *v) bases[nb++] = v; }
    static char darwin_tmp[PATH_MAX];
#ifdef _CS_DARWIN_USER_TEMP_DIR
    /* Only when none of the variables is set (a process started without
     * TMPDIR): this is the dir launchd gives Finder-started apps as TMPDIR.
     * confstr ignores the environment, so asking it while TMPDIR is set would
     * reach the real per-user dir even when a test points TMPDIR elsewhere. */
    if (!nb){
        if (!darwin_tmp[0] && confstr(_CS_DARWIN_USER_TEMP_DIR, darwin_tmp, sizeof darwin_tmp) == 0) darwin_tmp[0] = 0;
        if (darwin_tmp[0]) bases[nb++] = darwin_tmp;
    }
#endif
    bases[nb++] = "/tmp";
    /* Linux sandboxes (Flatpak, Snap) put the socket in a subdirectory; the
     * SDKs check these too.  Harmless on macOS. */
    static const char *const subs[] = { "", "app/com.discordapp.Discord/", "app/com.discordapp.DiscordCanary/",
                                        "snap.discord/", "snap.discord-canary/" };
    for (int b = 0; b < nb; b++){
        int dup = 0;
        for (int k = 0; k < b; k++){
            size_t lk = strlen(bases[k]), lb = strlen(bases[b]);
            while (lk && bases[k][lk-1] == '/') lk--;
            while (lb && bases[b][lb-1] == '/') lb--;
            if (lk == lb && !strncmp(bases[k], bases[b], lb)) dup = 1;
        }
        if (dup) continue;
        size_t bl = strlen(bases[b]);
        int slash = bl && bases[b][bl-1] == '/';
        for (size_t s = 0; s < sizeof subs / sizeof *subs; s++)
            for (int i = 0; i < 10 && n < max; i++){
                int w = snprintf(out[n], DS_SUN, "%s%s%sdiscord-ipc-%d", bases[b], slash ? "" : "/", subs[s], i);
                if (w > 0 && w < DS_SUN) n++;                    /* too long for a Unix socket: skip */
            }
    }
    return n;
}

static int conn_open(Conn *c){
    static char cand[400][DS_SUN];
    int n = ipc_candidates(cand, 400);
    for (int i = 0; i < n; i++){
        struct sockaddr_un sa; memset(&sa, 0, sizeof sa);
        sa.sun_family = AF_UNIX;
        if (strlen(cand[i]) >= sizeof sa.sun_path) continue;
        if (!g_ipc_path[0]){                    /* Discord's socket is a socket of ours (not one another user put in /tmp) */
            struct stat st;
            if (stat(cand[i], &st) != 0 || !S_ISSOCK(st.st_mode) || st.st_uid != getuid()) continue;
        }
        strcpy(sa.sun_path, cand[i]);
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        int one = 1;
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);   /* a vanished Discord must not kill us */
#endif
        (void)one;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0){ close(fd); continue; }
        c->fd = fd; c->ready = 0; c->rlen = 0;
        snprintf(c->path, sizeof c->path, "%s", cand[i]);
        char hs[96];
        snprintf(hs, sizeof hs, "{\"v\":1,\"client_id\":\"%s\"}", g_app_id);
        if (send_frame(c, OP_HANDSHAKE, hs) != 0){ conn_close(c, "handshake not sent"); continue; }
        c->hs_deadline = mono_s() + DS_READY_TMO;
        if (g_log) dlog("handshake sent to %s", c->path);
        return 0;
    }
    return -1;
}

/* Tiny tolerant JSON field readers (the payloads are small and flat enough). */
static const char *json_find(const char *j, const char *key){
    char k[40]; snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(j, k);
    if (!p) return 0;
    p += strlen(k);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}
static void json_get_str(const char *j, const char *key, char *out, size_t osz){
    out[0] = 0;
    const char *p = json_find(j, key);
    if (!p || *p != '"') return;
    size_t o = 0;
    for (p++; *p && *p != '"' && o + 1 < osz; p++){
        if (*p == '\\' && p[1]){ p++; out[o++] = *p == 'n' ? ' ' : *p; continue; }
        out[o++] = (char)((unsigned char)*p < 0x20 ? ' ' : *p);
    }
    out[o] = 0;
}
static long json_get_int(const char *j, const char *key, long def){
    const char *p = json_find(j, key);
    if (!p || !(*p == '-' || (*p >= '0' && *p <= '9'))) return def;
    return strtol(p, 0, 10);
}

/* Error notes are logged once per distinct text (a retry every 20 s would
 * otherwise repeat the same line). */
static void note_error(const char *what, long code, const char *msg){
    static char last[300];
    char line[300];
    snprintf(line, sizeof line, "Discord refused the %s: code %ld \"%s\"", what, code, msg);
    if (!strcmp(line, last)) return;
    snprintf(last, sizeof last, "%s", line);
    dlog("%s", line);
}

static int g_last_close_code;          /* last CLOSE code from Discord (4000: bad client id ...) */

/* Handle one complete frame.  Returns -1 when the connection must go. */
static int handle_frame(Conn *c, uint32_t op, char *json, size_t len){
    json[len] = 0;
    for (size_t i = 0; i < len; i++) if (!json[i]) json[i] = ' ';   /* embedded NULs: keep the scanners safe */
    switch (op){
    case OP_FRAME: {
        char evt[32], cmd[32];
        json_get_str(json, "evt", evt, sizeof evt);
        json_get_str(json, "cmd", cmd, sizeof cmd);
        if (g_log >= 2) dlog("<- op 1 cmd=%s evt=%s%s", cmd, evt[0] ? evt : "null",
                             strcmp(evt, "READY") ? "" : " (payload not logged)");
        if (!strcmp(cmd, "DISPATCH") && !strcmp(evt, "READY")){
            if (!c->ready) dlog("connected to Discord (%s)", c->path);
            c->ready = 1; g_last_close_code = 0;
            return 0;
        }
        if (!strcmp(evt, "ERROR")){
            const char *d = json_find(json, "data");
            char msg[200]; json_get_str(d ? d : json, "message", msg, sizeof msg);
            note_error(cmd[0] ? cmd : "request", json_get_int(d ? d : json, "code", 0), msg);
            return c->ready ? 0 : -1;            /* an error before READY: the handshake failed */
        }
        return 0; }                              /* replies to our SET_ACTIVITY: nothing to do */
    case OP_CLOSE: {
        char msg[200]; json_get_str(json, "message", msg, sizeof msg);
        g_last_close_code = (int)json_get_int(json, "code", 0);
        note_error(c->ready ? "connection" : "handshake", g_last_close_code, msg);
        return -1; }
    case OP_PING:
        return send_frame(c, OP_PONG, json) == 0 ? 0 : -1;
    case OP_PONG:
        return 0;
    default:
        dlog("protocol error: unknown opcode %u, reconnecting later", op);
        return -1;
    }
}
/* Read what is available; parse complete frames.  -1: connection gone. */
static int conn_read(Conn *c){
    for (;;){
        if (c->rlen >= sizeof c->rbuf){ dlog("protocol error: frame too large"); return -1; }
        ssize_t r = recv(c->fd, c->rbuf + c->rlen, sizeof c->rbuf - c->rlen, 0);
        if (r == 0) return -1;                         /* Discord closed the socket */
        if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
        c->rlen += (size_t)r;
        size_t off = 0;
        while (c->rlen - off >= 8){
            const uint8_t *h = c->rbuf + off;
            uint32_t op  = h[0] | h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
            uint32_t len = h[4] | h[5] << 8 | (uint32_t)h[6] << 16 | (uint32_t)h[7] << 24;
            if (op > OP_PONG || len > DS_MAXFRAME - 1){ dlog("protocol error: bad frame header (op %u, length %u)", op, len); return -1; }
            if (c->rlen - off - 8 < len) break;
            char *j = malloc(len + 1);
            if (!j) return -1;
            memcpy(j, h + 8, len);
            int rc = handle_frame(c, op, j, len);
            free(j);
            off += 8 + len;
            if (rc < 0) return -1;
        }
        if (off){ memmove(c->rbuf, c->rbuf + off, c->rlen - off); c->rlen -= off; }
    }
}

/* Bounded append (an overlong result is caught by the caller via *o >= sz). */
static void jcat(char *b, size_t sz, size_t *o, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void jcat(char *b, size_t sz, size_t *o, const char *fmt, ...){
    if (*o >= sz) return;
    va_list ap; va_start(ap, fmt);
    int w = vsnprintf(b + *o, sz - *o, fmt, ap);
    va_end(ap);
    *o = w < 0 ? sz : *o + (size_t)w;
}
/* SET_ACTIVITY with the snapshot p (p->active == 0: clear). */
static int send_activity(Conn *c, const Presence *p){
    char j[8192], t[DS_TXT * 6 + 8];
    size_t o = 0;
    jcat(j, sizeof j, &o, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%d", (int)getpid());
    if (p->active){
        const char *sep = "";
        jcat(j, sizeof j, &o, ",\"activity\":{");
        if (p->details[0] && json_str(t, sizeof t, p->details)){ jcat(j, sizeof j, &o, "%s\"details\":%s", sep, t); sep = ","; }
        if (p->state[0] && json_str(t, sizeof t, p->state)){ jcat(j, sizeof j, &o, "%s\"state\":%s", sep, t); sep = ","; }
        if (p->start > 0){ jcat(j, sizeof j, &o, "%s\"timestamps\":{\"start\":%lld}", sep, (long long)p->start); sep = ","; }
        if (g_large_image[0]){
            char li[sizeof g_large_image * 6 + 8];
            if (json_str(li, sizeof li, g_large_image)){
                jcat(j, sizeof j, &o, "%s\"assets\":{\"large_image\":%s,\"large_text\":\"Need for Speed Underground 2\"}", sep, li);
                sep = ","; }
        }
        jcat(j, sizeof j, &o, "%s\"instance\":false}", sep);
    }
    jcat(j, sizeof j, &o, "},\"nonce\":\"%u\"}", ++c->nonce);
    if (o >= sizeof j) return 0;                        /* cannot happen with clamped texts: skip, keep the link */
    if (g_log && !g_minimal) dlog("presence -> %s%s%s", p->active ? p->details : "(cleared)",
                                  p->state[0] ? " | " : "", p->state);
    else if (g_log) dlog("presence -> %s", p->active ? "(minimal)" : "(cleared)");
    return send_frame(c, OP_FRAME, j);
}

static void *ipc_main(void *arg){
    (void)arg;
    pthread_setname_np("discord ipc");
    Conn *c = &g_c;
    Presence want = { 0 }, sent = { 0 };
    int have_sent = 0;                         /* "sent" is what Discord shows for this connection */
    unsigned seen_seq = 0;
    int was_parked = 0; unsigned park_seq = 0; double unpark_t = 0;
    double next_try = 0, backoff = DS_RETRY_MIN;
    double sends[DS_RATE_N] = { 0 }; int si = 0;   /* ring of the last send times */
    double last_send = -1e9;

    while (!atomic_load(&g_stop)){
        double now = mono_s();
        /* latest snapshot */
        pthread_mutex_lock(&g_mu);
        if (g_pub_seq != seen_seq){ want = g_pub; seen_seq = g_pub_seq; }
        pthread_mutex_unlock(&g_mu);
        /* parked in the background: no new samples come, so the last one is
         * shown as paused, and after the resume until the game has sampled
         * again (its race timer re-anchored), at most DS_UNPARK_S */
        int parked = DS_PARKED() != 0;
        if (parked && !was_parked) park_seq = seen_seq;
        if (!parked && was_parked) unpark_t = now;
        was_parked = parked;
        if (unpark_t && (seen_seq != park_seq || now - unpark_t >= DS_UNPARK_S)) unpark_t = 0;
        Presence eff = want;
        if (parked || unpark_t) pres_parked(&eff);

        if (c->fd < 0 && now >= next_try){
            if (conn_open(c) == 0) have_sent = 0;
            else {
                next_try = now + backoff;
                backoff = backoff * 2 > DS_RETRY_MAX ? DS_RETRY_MAX : backoff * 2;
            }
        }
        if (c->fd >= 0 && !c->ready && now > c->hs_deadline){
            conn_close(c, "no READY from Discord");
            next_try = now + backoff; backoff = backoff * 2 > DS_RETRY_MAX ? DS_RETRY_MAX : backoff * 2;
        }
        double wait = DS_RETRY_MAX;
        if (c->fd >= 0 && c->ready){
            backoff = DS_RETRY_MIN;
            int differs = !have_sent ? eff.active : !pres_same(&eff, &sent);
            if (differs){
                int major = !have_sent || !pres_same_major(&eff, &sent);
                double oldest = sends[si];                 /* the send DS_RATE_N ago */
                double t_ok = oldest + DS_RATE_WIN;        /* rate window */
                if (!major && last_send + DS_MINOR_GAP > t_ok) t_ok = last_send + DS_MINOR_GAP;
                if (now >= t_ok){
                    if (send_activity(c, &eff) != 0){
                        conn_close(c, "send failed");
                        next_try = now + DS_RETRY_MIN;
                        continue;
                    }
                    sent = eff; have_sent = 1;
                    sends[si] = now; si = (si + 1) % DS_RATE_N; last_send = now;
                } else wait = t_ok - now;
            }
        } else if (c->fd >= 0) wait = c->hs_deadline - now;
        else wait = next_try - now;
        if (unpark_t && wait > unpark_t + DS_UNPARK_S - now) wait = unpark_t + DS_UNPARK_S - now;
        if (wait < 0.01) wait = 0.01;
        if (wait > DS_RETRY_MAX) wait = DS_RETRY_MAX;

        struct pollfd pf[2] = { { g_wake[0], POLLIN, 0 }, { c->fd, POLLIN, 0 } };
        int np = c->fd >= 0 ? 2 : 1;
        int r = poll(pf, (nfds_t)np, (int)(wait * 1000) + 1);
        if (r > 0 && (pf[0].revents & POLLIN)){ char b[64]; while (read(g_wake[0], b, sizeof b) > 0){} }
        if (r > 0 && np == 2 && (pf[1].revents & (POLLIN | POLLHUP | POLLERR))){
            if (conn_read(c) < 0){
                conn_close(c, c->ready ? "Discord closed the connection" : "handshake failed");
                /* a refused client id will not get better quickly: retry slowly */
                next_try = mono_s() + (g_last_close_code >= 4000 && g_last_close_code < 5000 ? DS_RETRY_MAX : backoff);
                backoff = backoff * 2 > DS_RETRY_MAX ? DS_RETRY_MAX : backoff * 2;
            }
        }
    }
    /* Clean quit: clear the activity, give Discord a moment to answer (any
     * reply frame), close.  Bounded: discord_shutdown waits DS_QUIT_WAIT_MS. */
    if (c->fd >= 0 && c->ready){
        Presence clr = { 0 };
        if (send_activity(c, &clr) == 0){
            double until = mono_s() + 0.3;
            size_t had = c->rlen;
            while (mono_s() < until){
                struct pollfd p = { c->fd, POLLIN, 0 };
                if (poll(&p, 1, 50) <= 0) continue;
                if (conn_read(c) < 0 || c->rlen != had) break;
                break;                                   /* a reply was parsed: Discord has it */
            }
            if (c->fd >= 0) send_frame(c, OP_CLOSE, "{}");
        }
    }
    conn_close(c, "quit");
    pthread_mutex_lock(&g_done_mu);
    atomic_store(&g_done, 1);
    pthread_cond_broadcast(&g_done_cv);
    pthread_mutex_unlock(&g_done_mu);
    return 0;
}

/* ======================================================================
 * Game thread
 * ====================================================================== */
static void ds_sample(Presence *p);          /* below */
void discord_trace_raw(void);
static int64_t g_session_start;
static double  g_cost_total, g_cost_max; static long g_cost_n;

void discord_shutdown(void);
static void ds_report(void){
    if (g_log && g_cost_n)
        dlog("sampler: %ld samples, avg %.1f us, max %.1f us (game thread)",
             g_cost_n, g_cost_total / g_cost_n * 1e6, g_cost_max * 1e6);
}
static void ds_atexit(void){ discord_shutdown(); ds_report(); }

/* Why this run is a test or diagnostic run rather than someone playing, or
 * NULL.  Only a windowed run without a time limit, script or test window is
 * a play session (run.sh and the app set WINDOW=1 and ALARM=0). */
static const char *automated_run(void){
    const char *v;
    if ((v = getenv("NFSU2_INPUT_SCRIPT")) && *v) return "NFSU2_INPUT_SCRIPT";
    if ((v = getenv("NFSU2_QUIT_FRAMES")) && *v) return "NFSU2_QUIT_FRAMES";
    if ((v = getenv("QUITFRAMES")) && *v) return "QUITFRAMES";
    if (!getenv("WINDOW")) return "headless";                       /* boot.c: no WINDOW, no window */
    if ((v = getenv("NOFOCUS")) && *v && *v != '0') return "NOFOCUS"; /* background test window */
    v = getenv("ALARM");                                            /* NFSU2_TIMEOUT lands here too */
    if (!v || atoi(v) != 0) return "a timeout (ALARM/NFSU2_TIMEOUT)";   /* boot.c: 20 s when unset */
    return 0;
}

static void ds_init(void){
    const char *on = getenv("NFSU2_DISCORD");
    g_on = env_bool(on, 1);
    const char *lg = getenv("NFSU2_DISCORD_LOG"); g_log = lg ? atoi(lg) : 0;
    if (!g_on){ if (g_log) dlog("off (NFSU2_DISCORD)"); return; }
    if (on && *on && env_bool(on, -1) < 0) dlog("NFSU2_DISCORD=\"%s\" is not on/off; treating it as on", on);
    const char *id = getenv("NFSU2_DISCORD_APP_ID");
    if (id && *id){
        int ok = strlen(id) < sizeof g_app_id;
        for (const char *q = id; *q && ok; q++) if (*q < '0' || *q > '9') ok = 0;
        if (!ok){ dlog("NFSU2_DISCORD_APP_ID=\"%s\" is not a numeric application id; Discord presence off", id); g_on = 0; return; }
        snprintf(g_app_id, sizeof g_app_id, "%s", id);
    } else snprintf(g_app_id, sizeof g_app_id, "%s", DS_DEFAULT_APP_ID);
    const char *ipc = getenv("NFSU2_DISCORD_IPC");
    if (ipc && *ipc){
        if (strlen(ipc) >= DS_SUN){ dlog("NFSU2_DISCORD_IPC is longer than a Unix socket path allows (%d); Discord presence off", DS_SUN - 1);
                                     g_on = 0; return; }
        snprintf(g_ipc_path, sizeof g_ipc_path, "%s", ipc);
    }
    const char *why = g_ipc_path[0] ? 0 : automated_run();
    if (why){
        /* A test or diagnostic run must never reach the real Discord app. */
        dlog("off: automated run (%s) without NFSU2_DISCORD_IPC", why);
        g_on = 0; return;
    }
    const char *det = getenv("NFSU2_DISCORD_DETAIL");
    g_minimal = det && !strcasecmp(det, "minimal");
    if (det && *det && !g_minimal && strcasecmp(det, "full"))
        dlog("NFSU2_DISCORD_DETAIL=\"%s\" unknown (full|minimal), using full", det);
    const char *li = getenv("NFSU2_DISCORD_LARGE_IMAGE");
    if (li && *li){
        /* an asset key or a URL: printable ASCII (a URL percent-encodes the rest) */
        int ok = strlen(li) < sizeof g_large_image;
        for (const char *q = li; *q && ok; q++) if (*q <= ' ' || *q > '~') ok = 0;
        if (ok) snprintf(g_large_image, sizeof g_large_image, "%s", li);
        else dlog("NFSU2_DISCORD_LARGE_IMAGE is not an asset key or URL (printable ASCII, < %d bytes); no image",
                  (int)sizeof g_large_image);
    }
    g_session_start = (int64_t)time(0);

    if (pipe(g_wake) != 0){ dlog("pipe failed; Discord presence off"); g_on = 0; return; }
    for (int i = 0; i < 2; i++){
        fcntl(g_wake[i], F_SETFL, fcntl(g_wake[i], F_GETFL) | O_NONBLOCK);
        fcntl(g_wake[i], F_SETFD, FD_CLOEXEC);
    }
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_set_qos_class_np(&at, QOS_CLASS_UTILITY, 0);   /* never compete with the game thread */
    if (pthread_create(&g_thr, &at, ipc_main, 0) != 0){
        dlog("no IPC thread; Discord presence off"); g_on = 0; pthread_attr_destroy(&at); return; }
    pthread_attr_destroy(&at);
    pthread_detach(g_thr);
    atomic_store(&g_started, 1);
    atexit(ds_atexit);
    dlog("on: app id %s, detail %s%s%s", g_app_id, g_minimal ? "minimal" : "full",
         g_ipc_path[0] ? ", socket " : "", g_ipc_path);
}

/* Called from Present on the game thread (guest lock held). */
void discord_on_present(void){
    if (__builtin_expect(g_on == 0, 1)) return;
    if (g_on < 0){ ds_init(); if (!g_on) return; }
    static double next_t, cand_since; static Presence cand, shown; static int have_shown;
    double t0 = mono_s();
    if (g_log >= 1){
        /* frame pacing as the game thread sees it (Present to Present), so a
         * test can show the IPC thread never stalls a frame */
        static double last, win0, wmax, wsum; static long wn;
        if (last > 0){ double f = t0 - last; wsum += f; wn++; if (f > wmax) wmax = f; }
        last = t0;
        if (win0 == 0) win0 = t0;
        if (t0 - win0 >= 5.0){
            struct timespec rt; clock_gettime(CLOCK_REALTIME, &rt);
            if (wn) dlog("frames: wall=%.3f n=%ld avg=%.2fms max=%.2fms", rt.tv_sec + rt.tv_nsec / 1e9, wn,
                         wsum / wn * 1e3, wmax * 1e3);
            win0 = t0; wmax = wsum = 0; wn = 0;
        }
    }
    if (t0 < next_t) return;
    next_t = t0 + 1.0 / DS_SAMPLE_HZ;

    Presence p; memset(&p, 0, sizeof p);
    ds_sample(&p);
    if (g_log >= 3){ static double tt; if (t0 >= tt){ tt = t0 + 2.0; discord_trace_raw(); } }
    utf8_clamp(p.details, sizeof p.details, DS_MAXCHARS);
    utf8_clamp(p.state, sizeof p.state, DS_MAXCHARS);
    /* settle: a new situation (phase, first line, timer) must hold
     * DS_SETTLE_MS; the second line alone (lap, position, points) passes
     * straight through and is paced by the IPC thread */
    if (!pres_same_major(&p, &cand)){ cand = p; cand_since = t0; }
    else cand = p;
    int settled = (t0 - cand_since) * 1000.0 >= DS_SETTLE_MS;
    int same_line1 = have_shown && pres_same_major(&cand, &shown);
    if ((settled || same_line1) && (!have_shown || !pres_same(&cand, &shown))){
        if (pthread_mutex_trylock(&g_mu) == 0){          /* never wait on the IPC thread */
            g_pub = cand; g_pub_seq++;
            pthread_mutex_unlock(&g_mu);
            shown = cand; have_shown = 1;
            wake_ipc();
            if (g_log >= 1) dlog("state: %s%s%s", cand.details, cand.state[0] ? " | " : "", cand.state);
        }
    }
    double dt = mono_s() - t0;
    g_cost_total += dt; g_cost_n++; if (dt > g_cost_max) g_cost_max = dt;
}

/* lifecycle.c, when the game is told it is active or inactive (parked in the
 * background): the IPC thread re-checks what to show.  Any thread. */
void discord_lifecycle_changed(void){
    if (atomic_load(&g_started)) wake_ipc();
}

/* Clean quit: clear the activity and close (bounded wait, never hangs). */
void discord_shutdown(void){
    if (!atomic_load(&g_started) || atomic_exchange(&g_stop, 1)) return;
    wake_ipc();
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += DS_QUIT_WAIT_MS * 1000000L; ts.tv_sec += ts.tv_nsec / 1000000000L; ts.tv_nsec %= 1000000000L;
    pthread_mutex_lock(&g_done_mu);
    while (!atomic_load(&g_done))
        if (pthread_cond_timedwait(&g_done_cv, &g_done_mu, &ts) != 0) break;
    pthread_mutex_unlock(&g_done_mu);
    if (!atomic_load(&g_done)) dlog("IPC thread did not finish in time; exiting anyway");
}


/* ======================================================================
 * Game state (game thread, guest memory; SPEED2.EXE v1.2 NA addresses,
 * found in the exe and checked at run time, see the doc for how)
 * ====================================================================== */
#define G_FLOW         0x008654a4u  /* GameFlowManager: 3 front end, 4/5 loading, 6 in the world */
#define G_RACE         0x0089e7a0u  /* race parameters, filled before a track loads: */
#define   R_TRACK        0x00u      /*   int  track number (4014 ...; n*1000 = free roam) */
#define   R_REVERSE      0x04u      /*   int  1: reversed (routes "B") */
#define   R_KIND         0x10u      /*   int  0 free roam, 1 race, 3 lap knockout */
#define   R_P2P          0x14u      /*   int  point to point (sprint, drag) */
#define   R_LAPS         0x1cu      /*   int  laps */
#define   R_DRAG         0x38u      /*   byte drag */
#define   R_DRIFT        0x39u      /*   byte drift */
#define   R_URL          0x41u      /*   byte URL (Underground Racing League) */
#define   R_SHORT        0x43u      /*   byte Street X ("HUD_Short_Track") */
#define   R_HUMAN        0x68u      /*   int  racer index of player 1 */
#define   R_RACERS       0x70u      /*   ptr  racer records, 0x940 bytes each; +0x10 car type */
#define   R_NRACERS      0x74u      /*   int  */
#define G_FE_MODE      0x00850078u  /* front-end game mode: 1 career */
#define G_STAGE        0x00862838u  /* career stage (career +0x7AF8): 0 prologue, 1..5, 6 = career complete */
#define G_PROFILE      0x0083a9e0u  /* player 1 profile name, char[16] */
#define G_LANG_N       0x008383d0u  /* localised strings: count */
#define G_LANG_TAB     0x008383d8u  /*   sorted {u32 hash, char *text} */
#define G_TRACKS       0x00883da0u  /* TrackInfo[], 0x128 bytes; +0x8a int16 number */
#define G_TRACKS_N     0x00883da4u
#define G_CARTYPES     0x008a1cccu  /* CarTypeInfo[], 0x890 bytes; +0 name ("CAR_NAME_<name>"), +0xc0 make, +0xd0 name hash */
#define G_CARTYPES_N   0x2eu        /*   46 entries (0x610130's loop bound) */
#define G_PLAYER1      0x008900acu  /* Player*; +4 its car */
#define G_RSTATUS      0x00890118u  /* race status: +0 state (3 countdown, 4 racing), +0x14 clock (1/4000 s), */
#define   RS_COUNT       0x24u      /*   racer entries in use: the game fills entry [+0x24] and then */
                                    /*   counts it (0x6018ac); the entries past it are never written and */
                                    /*   hold whatever the memory held before (0xEE fill from the game's */
                                    /*   own pool with the real heap) */
#define   RS_ENTRY       0x30u      /*   6 x 0x1f0 racer entries: +0 car, +9 laps done (int8), +0xa position, */
#define   RS_ENTRY_SZ    0x1f0u     /*   +0x140 drift points per lap (float), +0x190 banked total */

const char *autotest_top_fng(void);             /* autotest.c: top front-end package */
const char *autotest_screen(void);              /* autotest.c: current front-end screen */
const char *autotest_screen_batch(int i);       /* autotest.c: the i-th screen initialised with it */
int roam_game_friends(void);                    /* roam_game.c: free-roam friends connected (0 = off) */
int winsock_session_active(void);               /* winsock.c: a LAN session is open (the game's WSAStartup
                                                   on its LAN screens up to the last WSACleanup) */
/* A network race: World::IsOnline (0x5F00F0), as framerate.c tests it:
 * World+0x21 online flag, client / host race state > 0. */
#define G_NET_ONLINE   0x0089cf69u
#define G_NET_CLIENT   0x008669f4u
#define G_NET_HOST     0x00866a14u

static int committed_range(uint32_t a, uint32_t n){
    if (!n) return 1;
    if (a + n < a) return 0;
    for (uint32_t pg = a >> 14; pg <= (a + n - 1) >> 14; pg++)
        if (!mem_is_committed(pg << 14)) return 0;
    return 1;
}
static uint32_t R32(uint32_t a){ return committed_range(a, 4) ? MR32(a) : 0; }
static uint8_t  R8(uint32_t a){ return committed_range(a, 1) ? MR8(a) : 0; }
/* Copy a NUL-terminated guest string (at most max-1 bytes); "" if unreadable
 * or not terminated. */
static void gstr(uint32_t a, uint8_t *out, size_t max){
    out[0] = 0;
    if (!a) return;
    for (size_t i = 0; i + 1 < max; i++){
        if (!committed_range(a + (uint32_t)i, 1)){ out[0] = 0; return; }
        out[i] = MR8(a + (uint32_t)i);
        if (!out[i]) return;
    }
    out[0] = 0;                                   /* too long: not a string we want */
}
static uint32_t bhash(const char *s){                     /* bStringHash (0x43db50) */
    uint32_t h = 0xFFFFFFFFu;
    for (; *s; s++) h = h * 33u + (uint8_t)*s;
    return h;
}
/* Localised string for a key (GetLocalizedString 0x4ff9d0), as UTF-8. */
static int loc(const char *key, char *out, size_t osz){
    out[0] = 0;
    uint32_t n = R32(G_LANG_N), tab = R32(G_LANG_TAB), h = bhash(key);
    if (!n || n > 200000 || !committed_range(tab, n * 8u)) return 0;
    uint32_t lo = 0, hi = n;
    while (lo < hi){
        uint32_t mid = (lo + hi) / 2, mh = MR32(tab + mid * 8u);
        if (mh == h){
            uint8_t raw[256]; gstr(MR32(tab + mid * 8u + 4u), raw, sizeof raw);
            guest_to_utf8(out, osz, raw, sizeof raw);
            return out[0] != 0;
        }
        if (mh < h) lo = mid + 1; else hi = mid;
    }
    return 0;
}
static void track_name(uint32_t num, int reverse, char *out, size_t osz){
    out[0] = 0;
    uint32_t tab = R32(G_TRACKS), n = R32(G_TRACKS_N);
    if (!n || n > 2000 || !committed_range(tab, n * 0x128u)) return;
    for (uint32_t i = 0; i < n; i++){
        int16_t id = (int16_t)(MR32(tab + i * 0x128u + 0x88u) >> 16);   /* int16 at +0x8a */
        if ((uint32_t)(int32_t)id != num) continue;
        char key[48];
        if (reverse){
            snprintf(key, sizeof key, "REVERSE_TRACK_NAME_%d", id);
            if (loc(key, out, osz)) return;
        }
        snprintf(key, sizeof key, "TRACK_NAME_%d", id);
        loc(key, out, osz);
        return;
    }
}
/* "<Make> <Model>" for a car type: the model from the game's own localised
 * string CAR_NAME_<name> (as 0x512080 builds it), the make from the
 * manufacturer id (CarTypeInfo +0 name, +0xc0 manufacturer, 0x610170). */
static void car_type_name(uint32_t type, char *out, size_t osz){
    out[0] = 0;
    uint32_t tab = R32(G_CARTYPES);
    if (!tab || type >= G_CARTYPES_N) return;
    uint8_t nm[32], mf[32];
    gstr(tab + type * 0x890u, nm, sizeof nm);
    gstr(tab + type * 0x890u + 0xc0u, mf, sizeof mf);
    if (!nm[0]) return;
    for (const uint8_t *q = nm; *q; q++) if (*q < 0x20 || *q > 0x7e) return;
    for (const uint8_t *q = mf; *q; q++) if (*q < 0x20 || *q > 0x7e){ mf[0] = 0; break; }
    char key[64], model[96] = "", make[40] = "";
    snprintf(key, sizeof key, "CAR_NAME_%s", (const char *)nm);
    if (!loc(key, model, sizeof model)) return;
    /* the make has no text string (CARSELECT_MANUFACTURER_<mfr> is a logo);
     * title-case the id: "PEUGEOT" -> "Peugeot" */
    for (size_t i = 0; mf[i] && i + 1 < sizeof make; i++){
        char ch = (char)mf[i];
        make[i] = (i == 0 || mf[i-1] == ' ' || mf[i-1] == '-' || mf[i-1] == '_') ? ch : (char)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
        make[i+1] = 0;
        if (ch == '_') make[i] = ' ';
    }
    /* some model strings already start with the make */
    if (make[0] && strncasecmp(model, make, strlen(make))) snprintf(out, osz, "%s %s", make, model);
    else snprintf(out, osz, "%s", model);
}
/* The car the front end has selected: career car (career mode) or player 1's
 * quick-race/customize car, looked up in the front-end car records. */
#define G_FE_CARS      0x0083ad90u  /* FEDatabase+0x3c0: +0x9bd8 0x5b record pointers; record +8 handle */
#define G_FE_CAR_QR    0x0083aab8u  /* player 1's car handle (quick race, customize) */
#define G_FE_CAR_CAR   0x00863480u  /* career car handle */
/* A record's car type, as its class's GetType (vtable slot 1) computes it:
 * stock cars (vtable 0x79aadc, 0x503a30) keep the index at +0x18; customized
 * cars (0x79b12c, 0x503ae0) keep the car type's name hash at +0x20, matched
 * against CarTypeInfo +0xd0 like 0x610130 does.  Other classes: unknown. */
static int fe_record_type(uint32_t r){
    uint32_t vt = R32(r);
    if (vt == 0x0079aadcu){ uint32_t t = R32(r + 0x18); return t < G_CARTYPES_N ? (int)t : -1; }
    if (vt == 0x0079b12cu){
        uint32_t tab = R32(G_CARTYPES), hash = R32(r + 0x20);
        if (!tab || !committed_range(tab, G_CARTYPES_N * 0x890u)) return -1;
        for (uint32_t i = 0; i < G_CARTYPES_N; i++) if (MR32(tab + i * 0x890u + 0xd0u) == hash) return (int)i;
    }
    return -1;
}
uint32_t garage_find(uint32_t db, uint32_t h);   /* the table's, then the garage's slots 6-10 */
static void fe_car(int career, char *out, size_t osz){
    out[0] = 0;
    uint32_t h = R32(career ? G_FE_CAR_CAR : G_FE_CAR_QR);
    if (!h || !committed_range(G_FE_CARS + 0x9bd8u, 0x5bu * 4u)) return;
    uint32_t r = garage_find(G_FE_CARS, h);
    if (r){ int t = fe_record_type(r); if (t >= 0) car_type_name((uint32_t)t, out, osz); }
}
static const char *ordinal(int n){
    static char b[16];
    const char *suf = (n % 100 >= 11 && n % 100 <= 13) ? "th" : n % 10 == 1 ? "st" : n % 10 == 2 ? "nd" : n % 10 == 3 ? "rd" : "th";
    snprintf(b, sizeof b, "%d%s", n, suf);
    return b;
}
static void thousands(long v, char *out, size_t osz){
    char t[32]; snprintf(t, sizeof t, "%ld", v < 0 ? -v : v);
    size_t n = strlen(t), o = 0;
    if (v < 0 && o + 1 < osz) out[o++] = '-';
    for (size_t i = 0; i < n && o + 2 < osz; i++){
        out[o++] = t[i];
        if ((n - 1 - i) % 3 == 0 && i + 1 < n) out[o++] = ',';
    }
    out[o] = 0;
}

/* Front-end packages that are drawn over a screen and do not change what the
 * player is doing. */
static int pkg_overlay(const char *s){
    static const char *const pre[] = { "GenericDialog", "GenericerDialog", "HelpDialog", "IG_GenericDialog",
        "UI_PC_Help_Bar", "Chyron", "ScreenPrintf", "Keyboard", "UI_VirtualKeyboard", "StyleAnimator",
        "UI_InGameDialog", "MU_UG_InGameDialogBox", "UI_SponsorPopup", "DiscError", "UI_DecalsOverlay",
        "UI_CustomHUDOverlay", "UI_IcePartsOverlay", "UI_NeonPartsOverlay", "Loading_Tips", 0 };
    for (int i = 0; pre[i]; i++) if (!strncasecmp(s, pre[i], strlen(pre[i]))) return 1;
    return 0;
}
static int pkg_in(const char *s, const char *const *list){
    if (!s) return 0;
    for (int i = 0; list[i]; i++){
        size_t n = strlen(list[i]);
        if (list[i][n-1] == '*' ? !strncasecmp(s, list[i], n - 1) : !strcasecmp(s, list[i])) return 1;
    }
    return 0;
}
static const char *const k_title[]   = { "UG_LS_Splash.fng", "MW_LS_Splash.fng", 0 };
static const char *const k_movie[]   = { "IG_PlayMovie.fng", 0 };
static const char *const k_custom[]  = { "UI_ChooseCustomizeCategory.fng", "UI_ChoosePaintCategory.fng",
    "UI_CHoosePaintCategory.fng", "UI_ChoosePerformanceCategory.fng", "UI_ChoosePerformancePackage.fng",
    "UI_ChooseRimBrand.fng", "UI_ChooseSpinner.fng", "UI_ChooseUniquePart.fng", "UI_ChooseVinylLayer.fng",
    "UI_ChooseCustomHUD.fng", "UI_CustomNeonMain.fng", "UI_DecalMain.fng", "UI_GenericParts_Browser.fng",
    "UI_ICEMAIN.fng", "UI_Paint.fng", "UI_PerformanceBrandSelect.fng", "UI_Performance*", "UI_Rims_Browser.fng",
    "UI_BuyPerformanceParts.fng", "UI_PC_Customize_Options.fng", "UI_ShowCase_Preview.fng", "UI_Showcase*", 0 };
static const char *const k_carlot[]  = { "UI_CareerCarLot.fng", "UI_CareerCarSelect.fng", 0 };
static const char *const k_pause[]   = { "UI_Pause.fng", "UI_PauseOptions.fng", "UI_PauseOptionsMain.fng", 0 };
static const char *const k_post[]    = { "UI_PostRace*", "UI_PostraceWorldMap.fng", 0 };
static const char *const k_loading[] = { "PC_Loading.fng", "HUD_World_Loading.fng", 0 };
/* LAN play: the server list, the LAN main
 * menu, the lobby, the car choice and the game room.  The game shares the
 * UI_OL* / PC_OL_* screens with EA Online (the main menu's Online item:
 * UI_OLISPConnect, UI_OLEALogin, ...), so they count as LAN only after the
 * LAN branch (a UI_PC_LAN* screen) was entered from the main menu (g_lan_flow). */
static const char *const k_lan_pc[]  = { "UI_PC_LAN*", 0 };
static const char *const k_lan[]     = { "UI_PC_LAN*", "UI_OL*", "PC_OL_*", 0 };
static const char *const k_lan_room[] = { "PC_OL_GameRoom.fng", 0 };
static const char *const k_lan_find[] = { "UI_PC_LAN_ServerSelect.fng", 0 };
static const char *const k_lan_car[]  = { "UI_OLCarSelect.fng", 0 };
static const char *const k_qr_setup[] = { "UI_QRModeSelect.fng", "UI_QRTrackSelect.fng", "UI_QRModeOptions.fng", 0 };

static char  g_screen[64];         /* last front-end screen that is not an overlay */
static char  g_screen_raw[64];     /* last screen event seen (any package) */
static int   g_past_boot;          /* the main menu (or the world) has been reached */
static int   g_lan_flow;           /* a UI_PC_LAN* screen since the last main menu */
static int64_t g_event_start;      /* race timer origin (Unix s), 0: not in a race */
static uint32_t g_event_track;

/* In-engine cutscenes (NIS): three scene slots {+0 name hash, +8 active}; the
 * game itself tests the three "active" words together (0x45c012).  Seen set
 * for the career intro ("CareerNis1") and clear in free roam. */
#define G_NIS_ACTIVE0  0x00827e98u
#define G_NIS_ACTIVE1  0x00827eb0u
#define G_NIS_ACTIVE2  0x00827ec8u
static int nis_playing(void){ return (R32(G_NIS_ACTIVE0) | R32(G_NIS_ACTIVE1) | R32(G_NIS_ACTIVE2)) != 0; }

static const char *race_type(uint32_t rp){
    if (R8(rp + R_DRAG))  return "Drag";
    if (R8(rp + R_DRIFT)) return "Drift";
    if (R8(rp + R_SHORT)) return "Street X";
    if (R8(rp + R_URL))   return "URL";
    if (R32(rp + R_KIND) == 3) return "Lap Knockout";
    if (R32(rp + R_P2P))  return "Sprint";
    return "Circuit";
}

/* Drift score of a racer entry: per-lap scores (float) at +0x140, one per lap,
 * the banked total of finished laps at +0x190.  The running total the HUD shows
 * is the finished laps plus the lap in progress. */
#define RS_DRIFT_LAP   0x140u
#define RS_DRIFT_TOTAL 0x190u
static float drift_points(uint32_t e, int laps_done){
    float sum = 0;
    int n = laps_done < 0 ? 0 : laps_done > 19 ? 19 : laps_done;
    for (int i = 0; i <= n; i++){
        float f; uint32_t fb = MR32(e + RS_DRIFT_LAP + 4u * (uint32_t)i); memcpy(&f, &fb, 4);
        if (f == f && f > 0 && f < 1e8f) sum += f;
    }
    return sum;
}
typedef struct { int ok, laps_done, pos, nracers, state, drift_rank; float drift; uint32_t clock; } RacerView;
/* Player 1's entry in the race status (matched by its car object). */
static RacerView player_status(void){
    RacerView v; memset(&v, 0, sizeof v);
    uint32_t rs = R32(G_RSTATUS), pl = R32(G_PLAYER1);
    if (!rs || !pl || !committed_range(rs, RS_ENTRY + 6 * RS_ENTRY_SZ)) return v;
    uint32_t car = R32(pl + 4), n = MR32(rs + RS_COUNT);
    if (n > 6) n = 6;
    v.state = (int)MR32(rs); v.clock = MR32(rs + 0x14u);
    float pts[6]; int me = -1;
    for (int i = 0; i < (int)n; i++){
        uint32_t e = rs + RS_ENTRY + (uint32_t)i * RS_ENTRY_SZ, owner = MR32(e);
        pts[i] = -1;
        if (!owner) continue;
        v.nracers++;
        pts[i] = drift_points(e, (int8_t)MR8(e + 9));
        if (owner != car || !car) continue;
        v.ok = 1; me = i;
        v.laps_done = (int8_t)MR8(e + 9);
        v.pos = MR8(e + 0xa);
        v.drift = pts[i];
    }
    if (v.pos < 1 || v.pos > v.nracers) v.ok = 0;
    /* in a drift event the HUD ranks by points (the position byte is track order) */
    v.drift_rank = 1;
    for (int i = 0; i < (int)n && me >= 0; i++) if (i != me && pts[i] > pts[me]) v.drift_rank++;
    return v;
}
/* "Career — Stage N" as the game numbers its stages: [G_STAGE] is 0 in the
 * prologue (up to the first car bought), 1..5 while the STAGE_n_ / Sn_ events
 * of that stage run, 6 once the final race against Caleb is won. */
static void career_stage(char *out, size_t osz){
    uint32_t s = R32(G_STAGE);
    if (s == 0) snprintf(out, osz, "Career \xE2\x80\x94 Prologue");
    else if (s <= 5) snprintf(out, osz, "Career \xE2\x80\x94 Stage %u", s);
    else snprintf(out, osz, "Career \xE2\x80\x94 Complete");
}
static void player_car(char *out, size_t osz){
    out[0] = 0;
    uint32_t n = R32(G_RACE + R_NRACERS), hi = R32(G_RACE + R_HUMAN), base = R32(G_RACE + R_RACERS);
    if (!base || hi >= n || n > 32) return;
    car_type_name(R32(base + hi * 0x940u + 0x10u), out, osz);
}

static void ds_sample(Presence *p){
    p->active = 1;
    p->start = g_session_start;
    if (g_minimal){ p->phase = PH_NONE; return; }   /* "Playing" + elapsed time only */

    uint32_t flow = R32(G_FLOW);
    const char *top = autotest_top_fng();
    const char *scr = autotest_screen();
    if (scr && strcmp(scr, g_screen_raw)){            /* a new screen event */
        snprintf(g_screen_raw, sizeof g_screen_raw, "%s", scr);
        /* GarageMain.fng is the garage backdrop the game initialises in the
         * same frame as the screen in front of it (UI_CareerCarLot,
         * GarageMain, Chyron_FE at the car lot; UI_Main, GarageMain on the
         * way back to the main menu): that other screen is the one shown */
        const char *pick = 0, *b;
        for (int i = 0; (b = autotest_screen_batch(i)); i++)
            if (!pkg_overlay(b) && strcasecmp(b, "GarageMain.fng")) pick = b;
        if (!pick && !pkg_overlay(scr)) pick = scr;
        if (pick) snprintf(g_screen, sizeof g_screen, "%s", pick);
    }
    const char *cur = g_screen[0] ? g_screen : top;
    if (flow == 6 || pkg_in(cur, (const char *const[]){ "UI_Main.fng", "GarageMain.fng", "MC_Main.fng", 0 }))
        g_past_boot = 1;
    if (pkg_in(cur, k_lan_pc)) g_lan_flow = 1;
    else if (!winsock_session_active() && pkg_in(cur, (const char *const[]){ "UI_Main.fng", "MC_Main.fng", 0 }))
        g_lan_flow = 0;
    int lan = g_lan_flow && (winsock_session_active() || pkg_in(cur, k_lan));

    char *d = p->details, *st = p->state;
    const size_t dz = sizeof p->details, sz = sizeof p->state;
    uint32_t rp = G_RACE, track = R32(rp + R_TRACK), kind = R32(rp + R_KIND);
    int career = R32(G_FE_MODE) == 1;
    char tname[160] = "", car[160] = "";

    /* Movies first, they run in any flow state.  A story FMV is the current
     * screen while the front end runs (flow 3), with PC_Loading on top; with
     * movies off it is skipped within a frame or two, which the settle time
     * hides. */
    if (pkg_in(top, k_movie) || (pkg_in(cur, k_movie) && flow == 3)){
        p->phase = PH_MOVIE; snprintf(d, dz, "Watching a cutscene"); return; }
    if (!g_past_boot){
        int title = pkg_in(cur, k_title) || pkg_in(top, k_title);
        p->phase = title ? PH_TITLE : PH_BOOT;
        snprintf(d, dz, "%s", title ? "At the title screen" : "Starting up");
        return;
    }
    if (flow >= 4 && flow <= 8) g_screen[0] = 0;  /* a track loads or runs: earlier front-end screens are stale */
    if ((flow >= 1 && flow <= 8 && flow != 3 && flow != 6) || pkg_in(top, k_loading)){
        p->phase = PH_LOADING;
        snprintf(d, dz, "Loading\xE2\x80\xA6");
        if (career) snprintf(st, sz, "Career");
        else if (kind && track % 1000){
            track_name(track, R32(rp + R_REVERSE) == 1, tname, sizeof tname);
            snprintf(st, sz, "%s%s%s%s", lan ? "LAN race \xE2\x80\x94 " : "", race_type(rp),
                     tname[0] ? " \xE2\x80\x94 " : "", tname);
        }
        /* a new track: the next race gets its own timer.  Not for PC_Loading
         * on top while the world runs (flow 6): the game shows it for a few
         * frames when a race ends, and the results must keep the race timer
         * (after the finish the clock is no longer used to re-anchor it). */
        if (flow != 6) g_event_start = 0;
        return;
    }
    int netrace = R8(G_NET_ONLINE) || (int32_t)R32(G_NET_CLIENT) > 0 || (int32_t)R32(G_NET_HOST) > 0;
    if (flow == 6){
        /* a network race does not pause: its pause menu ("Quit to LAN Main
         * Menu") leaves the race running underneath */
        int paused = !netrace && pkg_in(top, k_pause);
        if (kind == 0 || track % 1000 == 0){                    /* free roam */
            g_event_start = 0;
            if (nis_playing()){ p->phase = PH_MOVIE; snprintf(d, dz, "Watching a cutscene"); return; }
            p->phase = paused ? PH_PAUSED : career ? PH_ROAM : PH_FREERUN;
            if (career){
                career_stage(d, dz);
                uint8_t raw[16]; gstr(G_PROFILE, raw, sizeof raw);
                char prof[64]; guest_to_utf8(prof, sizeof prof, raw, sizeof raw);
                /* runtime/language.c: the game's "No Profile" in its language, in the game's bytes */
                int language_no_profile(const char *name);
                char rawz[17]; memcpy(rawz, raw, 16); rawz[16] = 0;
                /* free-roam multiplayer (roam_game.c):
                 * with friends connected, that is what the second line says */
                int friends = paused ? 0 : roam_game_friends();
                if (paused) snprintf(st, sz, "Paused");
                else if (friends > 0) snprintf(st, sz, "Free roam with %d friend%s", friends, friends == 1 ? "" : "s");
                else if (prof[0] && !language_no_profile(rawz)) snprintf(st, sz, "Free roaming as %s", prof);
                else snprintf(st, sz, "Free roaming");
            } else {
                snprintf(d, dz, "Free Run");
                if (paused) snprintf(st, sz, "Paused");
                else { player_car(car, sizeof car); if (car[0]) snprintf(st, sz, "%s", car); }
            }
            return;
        }
        /* a race */
        track_name(track, R32(rp + R_REVERSE) == 1, tname, sizeof tname);
        const char *rt = race_type(rp);
        snprintf(d, dz, "%s%s%s%s", netrace ? "LAN race \xE2\x80\x94 " : career ? "Career " : "", rt,
                 tname[0] ? " \xE2\x80\x94 " : "", tname);
        RacerView v = player_status();
        int64_t now = (int64_t)time(0);
        if (g_event_start == 0 || track != g_event_track){
            g_event_start = now; g_event_track = track; }
        if (v.ok && v.state < 4 && !v.clock){                   /* intro camera, countdown */
            p->start = g_event_start;
            p->phase = paused ? PH_PAUSED : PH_GRID;
            snprintf(st, sz, "%s", paused ? "Paused" : "On the starting line");
            return;
        }
        uint32_t laps = R32(rp + R_LAPS);
        int finished = (v.ok && laps && v.laps_done >= (int)laps) || pkg_in(top, k_post) || pkg_in(cur, k_post);
        if (v.ok && v.clock && !paused && !finished){           /* anchor on the race clock (restarts, pauses); */
            int64_t s = now - (int64_t)(v.clock / 4000u);       /* frozen while paused or finished, where the  */
            if (s < g_event_start - 3 || s > g_event_start + 3) g_event_start = s;   /* clock may stop */
        }
        p->start = g_event_start;
        if (finished){
            p->phase = PH_FINISHED;
            if (v.ok && v.nracers > 1)
                snprintf(st, sz, "Finished %s of %d", ordinal(!strcmp(rt, "Drift") ? v.drift_rank : v.pos), v.nracers);
            else snprintf(st, sz, "Race results");
            return;
        }
        if (paused){ p->phase = PH_PAUSED; snprintf(st, sz, "Paused"); return; }
        p->phase = PH_RACE;
        if (!v.ok) return;
        char pts[32];
        if (!strcmp(rt, "Drift")){
            thousands((long)v.drift, pts, sizeof pts);
            char rk[32] = "";
            if (v.nracers > 1) snprintf(rk, sizeof rk, "%s of %d \xC2\xB7 ", ordinal(v.drift_rank), v.nracers);
            if (laps > 1) snprintf(st, sz, "Lap %d/%u \xC2\xB7 %s%s pts", v.laps_done + 1, laps, rk, pts);
            else snprintf(st, sz, "%s%s pts", rk, pts);
        } else if (laps > 1 && v.nracers > 1)
            snprintf(st, sz, "Lap %d/%u \xC2\xB7 %s of %d", v.laps_done + 1, laps, ordinal(v.pos), v.nracers);
        else if (v.nracers > 1)
            snprintf(st, sz, "%s of %d", ordinal(v.pos), v.nracers);
        return;
    }
    /* front end */
    g_event_start = 0;
    if (lan){
        p->phase = PH_LAN;
        snprintf(d, dz, "LAN lobby");
        if (pkg_in(cur, k_lan_room)) snprintf(st, sz, "In a game room");
        else if (pkg_in(cur, k_lan_find)) snprintf(st, sz, "Looking for games");
        else if (pkg_in(cur, k_lan_car)) snprintf(st, sz, "Choosing a car");
        else if (pkg_in(cur, k_qr_setup)) snprintf(st, sz, "Creating a game");
        else if (pkg_in(cur, k_post)) snprintf(st, sz, "Race results");
        return;
    }
    if (pkg_in(cur, k_custom)){
        p->phase = PH_CUSTOM;
        snprintf(d, dz, "Customizing");
        fe_car(career, car, sizeof car);
        if (car[0]) snprintf(st, sz, "%s", car);
        return;
    }
    if (pkg_in(cur, k_carlot) && career){ p->phase = PH_CARLOT; snprintf(d, dz, "Choosing a car"); return; }
    if (pkg_in(cur, k_post)){ p->phase = PH_RESULTS; snprintf(d, dz, "Race results"); return; }
    p->phase = PH_MENUS;
    snprintf(d, dz, "In the menus");
    if (career) career_stage(st, sz);
}

/* Debug: the raw inputs of the classification (NFSU2_DISCORD_LOG=3). */
void discord_trace_raw(void){
    if (g_log < 3) return;
    const char *top = autotest_top_fng(), *scr = autotest_screen();
    RacerView v = player_status();
    char car[128]; player_car(car, sizeof car);
    char fcar[128]; fe_car(R32(G_FE_MODE) == 1, fcar, sizeof fcar);
    char tn[128]; track_name(R32(G_RACE + R_TRACK), R32(G_RACE + R_REVERSE) == 1, tn, sizeof tn);
    dlog("raw: flow=%u top=%s screen=%s fe_mode=%u stage=%u track=%u (%s) kind=%u laps=%u car=\"%s\" "
         "rs=%d/%d pos=%d laps_done=%d racers=%d drift=%.0f clock=%u fecar=\"%s\" nis=%d",
         R32(G_FLOW), top ? top : "-", scr ? scr : "-", R32(G_FE_MODE), R32(G_STAGE),
         R32(G_RACE + R_TRACK), tn, R32(G_RACE + R_KIND), R32(G_RACE + R_LAPS), car,
         v.ok, v.state, v.pos, v.laps_done, v.nracers, v.drift, v.clock, fcar, nis_playing());
    uint32_t rs = R32(G_RSTATUS);
    if (rs && committed_range(rs, RS_ENTRY + 6 * RS_ENTRY_SZ)){
        char o[256]; size_t n = 0;
        int cnt = (int)MR32(rs + RS_COUNT); if (cnt < 0 || cnt > 6) cnt = 6;
        for (int i = 0; i < cnt && n < sizeof o - 40; i++){
            uint32_t e = rs + RS_ENTRY + (uint32_t)i * RS_ENTRY_SZ;
            if (!MR32(e)) continue;
            float tot; uint32_t tb = MR32(e + RS_DRIFT_TOTAL); memcpy(&tot, &tb, 4);
            n += (size_t)snprintf(o + n, sizeof o - n, " [%d] pos=%u laps=%d pts=%.0f banked=%.0f%s", i, MR8(e + 0xa),
                                  (int8_t)MR8(e + 9), (double)drift_points(e, (int8_t)MR8(e + 9)), (double)tot,
                                  MR32(e) == R32(R32(G_PLAYER1) + 4) ? " (player 1)" : "");
        }
        dlog("raw: race status%s", o);
    }
}
