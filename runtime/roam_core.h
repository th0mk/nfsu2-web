/* roam_core.h -- free-roam multiplayer: the portable core.
 *
 * Wire format (version 1), session (peers, handshake, gossip, timeouts, rate
 * limits, address policy), interpolation (clock offset, jitter buffer,
 * Hermite / nlerp, dead reckoning, error blending) and the car-look record.
 *
 * Portable C99 for the Mac port (arm64) and a Windows ASI (i686): no OS
 * headers, no libm, no threads, no allocation.  Every wire field goes
 * through explicit little-endian put/get helpers; no struct is ever copied
 * to or from the wire, so struct layout differences between LP64 and ILP32
 * do not matter.  Floats use only + - * / and sqrt (IEEE correctly rounded),
 * so a build with -ffp-contract=off gives the same bits on both platforms.
 *
 * Threading is the caller's business: every function that takes a session
 * must be called with the caller's session lock held.  Time is passed in as
 * microseconds of a monotonic clock (int64).
 */
#ifndef ROAM_CORE_H
#define ROAM_CORE_H
#include <stdint.h>
#include <stddef.h>

#define RC_PROTO_VER     1          /* header byte 4; a different value is ignored */
#define RC_CORE_VER      8          /* HELLO: implementation revision (2: lists of up to RC_MAX_PEERS; 3: RC_MSG_DUEL;
                                       4: state extensions, RC_HF_STATEX; 5: race lobbies, RC_HF_LOBBY; 6: quick chat,
                                       RC_HF_SAY; 7: crews, RC_HF_CREW; 8: crews from the hub, RC_HF_HUBCREW) */
#define RC_MAGIC0        0x4E       /* "N2FR" */
#define RC_MAGIC1        0x32
#define RC_MAGIC2        0x46
#define RC_MAGIC3        0x52
#define RC_DEFAULT_PORT  47210
#define RC_MAX_DGRAM     1200       /* never IP-fragmented (< 1280) */
#define RC_HDR           16
#define RC_CRC           4
#define RC_NAME_MAX      24
#define RC_MAX_PLAYERS   33         /* the mesh: this machine + 32 peers */
#define RC_MAX_PEERS     (RC_MAX_PLAYERS - 1)
#define RC_PEERS_V1      7          /* the most a core of version 1 reads in a HELLO's ids or a PEERS list */
#define RC_TABLE         64         /* known addresses (configured, gossiped, heard) */
#define RC_SNAP_STATES   3          /* states per SNAP (newest first): loss redundancy */
#define RC_STATE_BYTES   52
#define RC_STATEX_BYTES  29         /* a state's extension (RC_HF_STATEX), its length byte included, at most */
#define RC_STATEX_SLIP   24         /* ...without the engine (no engine sound; every core before it) */
#define RC_STATEX_HYD    12         /* ...without the wheels' slip (no wheel slides; every core before that) */
#define RC_STATEX_V4     10         /* ...as the first cores of version 4 sent it (no hydraulics) */
#define RC_STATEX_ENG    5          /* the engine's bytes, after the hydraulics or after the slip */
#define RC_SLIP_STEP     0.5f       /* a wheel's slip on the wire: int8 steps of this (m/s), up to 63.5 */
#define RC_BUF_STATES    32         /* jitter buffer per peer */
#define RC_OFFWIN        64         /* clock-offset samples per peer */
#define RC_CFG_MAX       2048       /* car-look record, bytes */
#define RC_CFG_FRAG      1000       /* bytes per CFG fragment */
#define RC_CFG_MAXFRAG   3
#define RC_PART_SLOTS    170        /* RideInfo part slots */
#define RC_CFG_TYPE_NAME 16
#define RC_PERF_BYTES    52         /* car-look record extension 1 */
#define RC_EXT_PERF      1
#define RC_EXT_HORN      2          /* car-look record extension 2: the player's horn, 1 byte */
#define RC_DUEL_INBOX    16         /* DUEL messages held for the game side */
#define RC_MAP_ENTRY     5          /* a MAP entry: x, y (int16, m), heading (u8, 256 a turn) */
#define RC_MAP_PER       230        /* entries per MAP datagram at most */
#define RC_MAP_PARTS     3          /* MAP datagrams per answer at most */
#define RC_MAP_MAX       512        /* entries per answer at most (the hub's nearest) */
#define RC_LOBBY_MAX     6          /* players in a race lobby: the game's network race takes 6 */
#define RC_LOBBY_INBOX   16         /* LOBBY messages held for the game side */
#define RC_LOBBIES_ENTRY 11         /* a LOBBIES entry: x, y (int16, m), race (u32), n, max, state (u8) */
#define RC_LOBBIES_MAX   96         /* entries per LOBBIES datagram (the hub's nearest) */
#define RC_RACEREP_BYTES 64         /* a RACEREP's payload: lobby, race (u32); mode, laps (u8); track (u16); place, players,
                                       flags, type, class (u8), 3 bytes 0; rating, field, factor, kW, kg, km/h, time (f32);
                                       the name (16 bytes, NUL padded) */
#define RC_RACEREP_NAME  16         /* its car type's name */
#define RC_RACERES_HEAD  12         /* a RACERES's payload: lobby, race (u32); mode, laps, n (u8), a byte 0; then n rows */
#define RC_RACERES_ROW   20         /* ...each: the player's id (u32), flags (u8), 3 bytes 0, its time, how far it came,
                                       the way it drove (f32) */
#define RC_SAY_INBOX     16         /* SAY messages held for the game side */

/* message types (header byte 5) */
enum { RC_MSG_HELLO = 1, RC_MSG_PEERS = 2, RC_MSG_SNAP = 3, RC_MSG_CFG = 4, RC_MSG_CFG_REQ = 5,
       RC_MSG_PING = 6, RC_MSG_PONG = 7, RC_MSG_BYE = 8, RC_MSG_DUEL = 9,
       /* the world map's other players: MAPREQ (req: our number for the
        * question) only to the hub, which answers with MAP datagrams (req
        * echoed; part of nparts; total: the players in our room past us and
        * our peers; n entries of RC_MAP_ENTRY bytes: the ones nearest to us,
        * past our peers, RC_MAP_MAX at most).  A hub before them ignores a
        * MAPREQ, and only a core that asked gets a MAP */
       RC_MSG_MAPREQ = 10, RC_MSG_MAP = 11,
       /* a multiplayer race from free roam (roam_game.c mp_*): LOBBY between
        * players (op RC_LOBBY_*), only to a peer whose HELLO carries
        * RC_HF_LOBBY; the host's ADVERT also to the hub (to 0), which keeps
        * the open lobbies of each room and answers a MAPREQ of such a core
        * with one LOBBIES datagram after its MAP (req echoed; n entries of
        * RC_LOBBIES_ENTRY bytes, nearest first: where each lobby's race
        * starts, its race, players, most players and state) */
       RC_MSG_LOBBY = 12, RC_MSG_LOBBIES = 13,
       /* a player's report of a multiplayer race it raced (rc_racerep), only
        * to the hub, once back in free roam: the server logs it.  A hub before
        * it ignores the type, and no player is sent one */
       RC_MSG_RACEREP = 14,
       /* quick chat (roam_game.c say_*): one of the fixed phrases (a number:
        * each player reads it in their own language, nobody types), to every
        * established peer whose HELLO carries RC_HF_SAY; seq tells a repeat
        * from a new one.  Two more bytes, 0 for now */
       RC_MSG_SAY = 15,
       /* the race's results as the reporter's game showed them (rc_raceres), only to the hub, with its RACEREP: every
        * racer's place in them, by its player's id, its time and how far it came.  The server compares the players'
        * games' results for a racer far ahead of the others (a teleport to the finish, a speed hack).  A hub before it
        * ignores the type, and no player is sent one */
       RC_MSG_RACERES = 16 };
/* RACEREP flags */
#define RC_RR_HOST       0x01       /* the reporter hosted the race */
#define RC_RR_QUIT       0x02       /* ...left it with its pause menu's Quit */
/* RACERES row flags */
#define RC_RS_FINISHED   0x01       /* the racer finished (its time is the race's) */
#define RC_RS_OURS       0x02       /* the reporter's own row (one at most) */
/* LOBBY ops.  lobby: the host's id for it (nonzero); race: the career race's
 * name hash (race record +8, the same table in every game); x, y: where it
 * starts (m); ids: the players in it, the host first */
enum { RC_LOBBY_ADVERT = 1,         /* host -> each peer (and the hub): the lobby as it is now; again every second */
       RC_LOBBY_JOIN = 2,           /* player -> host: let me in */
       RC_LOBBY_LEAVE = 3,          /* player -> host: I am out; host -> player: you are out (arg: RC_LL_*) */
       RC_LOBBY_START = 4,          /* host -> each player in it: the race begins in t ms (ids: who races) */
       RC_LOBBY_CLOSE = 5,          /* host -> each peer (and the hub): the lobby is gone (arg: RC_LL_*) */
       RC_LOBBY__N };
enum { RC_LL_LEFT = 1, RC_LL_FULL = 2, RC_LL_GONE = 3, RC_LL_STARTED = 4, RC_LL__N };
enum { RC_LS_OPEN = 0, RC_LS_STARTING = 1, RC_LS_RACING = 2, RC_LS__N };
/* DUEL ops: an outrun between two players (roam_game.c).  Only to a peer
 * whose HELLO carries RC_HF_DUELS: an older core counts the type as bad */
enum { RC_DUEL_OFFER = 1,           /* I challenge you (duel: a new id) */
       RC_DUEL_ACCEPT = 2,          /* I take your challenge */
       RC_DUEL_START = 3,           /* the challenger's outrun runs: yours starts now */
       RC_DUEL_RESULT = 4,          /* arg 1: I won, 2: I lost */
       RC_DUEL_CANCEL = 5,          /* arg: RC_DC_* */
       RC_DUEL__N };
enum { RC_DC_WITHDRAWN = 1, RC_DC_BUSY = 2, RC_DC_LEFT = 3, RC_DC__N };
/* player status (HELLO and SNAP) */
enum { RC_ST_MENUS = 0, RC_ST_LOADING = 1, RC_ST_ROAM = 2, RC_ST_EVENT = 3, RC_ST_SHOP = 4,
       RC_ST_PAUSED = 5, RC_ST__N };
/* platform byte (HELLO) */
enum { RC_PLAT_MAC = 1, RC_PLAT_WIN = 2, RC_PLAT_TOOL = 3 };
/* state bits (SNAP) */
#define RC_BIT_BRAKE     0x01       /* brake lights */
#define RC_BIT_REVERSE   0x02
#define RC_BIT_NOS       0x04       /* nitrous is on (the extension's nos says how much is left) */
#define RC_BIT_HANDBRAKE 0x08
#define RC_BIT_LIGHTS    0x10
#define RC_BIT_HORN      0x20       /* the horn sounds (horn.c); older builds ignore it */
#define RC_BIT_PASS      0x40       /* drive through this car: its player's game lets every car through it now (near
                                       one of their open events, their cars are ghosts: roam_game.c soft_me); older
                                       builds ignore it */
#define RC_BIT_BUSY      0x80       /* this player takes no outrun now (at a door or an event, in a marker or a race:
                                       roam_game.c du_free); older builds ignore it */
/* header flags (SNAP): the peer gets every n-th snapshot (0 or 1: all); a
 * peer far away gets fewer (rc_push_state), and its gap count follows */
#define RC_HF_EVERY      0x000F
#define RC_FAR_M         300.f      /* past this a peer gets every 4th snapshot (5 a second) */
#define RC_VERYFAR_M     1000.f     /* and past this every 10th (2 a second) */
/* header flags (HELLO): this player takes outrun challenges (RC_MSG_DUEL);
 * an older core sends none and ignores them */
#define RC_HF_DUELS      0x0010
/* header flags (HELLO): this player takes race lobbies (RC_MSG_LOBBY, and
 * from the hub RC_MSG_LOBBIES); an older core sends none and ignores them */
#define RC_HF_LOBBY      0x0080
/* header flags (HELLO): to everyone, not to one peer (the beacon, the hub):
 * it names at most RC_PEERS_V1 of the ids its sender hears, so a full list
 * without ours does not say the sender no longer hears us (its HELLOs to us
 * do); an older core sends none and ignores it */
#define RC_HF_ALL        0x0020
/* header flags (HELLO): this player reads state extensions; (SNAP): every
 * state is followed by its extension: a length byte (what follows, at least
 * RC_STATEX_V4 - 1), the wheel places' heights in the car (4 x int16, mm)
 * and the nitrous left (u8, see rc_state), then (RC_STATEX_HYD) the
 * hydraulics and the landings (u8 each), then (RC_STATEX_SLIP, only while a
 * wheel slides: its slip is at least half a step) each wheel's slip along
 * and across it (2 x int8, RC_SLIP_STEP) and its surface (u8), then (only
 * while the car's engine sounds) the engine (RC_STATEX_ENG: its sound's rpm,
 * u16, and its three voices' volumes, u8 each, see rc_state), so 12, 17, 24
 * or 29 bytes: the length says which; whatever a newer core adds comes
 * after the engine of the longest form, and is skipped.  Only to a peer
 * whose HELLO carries the flag: an older core counts the longer SNAP as bad */
#define RC_HF_STATEX     0x0040
/* header flags (HELLO): this player takes quick chat (RC_MSG_SAY); an older
 * core sends none and counts the type as bad */
#define RC_HF_SAY        0x0100
/* header flags (HELLO): this player reads crews.  A HELLO with it may end,
 * after its ids, in its crews (the Crews setting): a count byte, at most
 * RC_CREW_MAX, then that many rc_room_hash values (u32, none 0); whatever a
 * newer core adds after them is skipped.  The crews go to the hub; to a peer
 * whose HELLO carries the flag (an older core counts the longer HELLO as
 * bad) only without a hub (a LAN of its own), and never in the beacon.
 * Players who share a crew are linked first by the hub, wherever they are,
 * and see each other in the crew's colour (roam_game.c).
 * On a PEERS from the hub the same flag says the list ends in crew marks
 * (only to a player whose HELLO carries RC_HF_HUBCREW): after the entries, the
 * receiver's own crews as the hub has them (a count byte, at most
 * RC_CREW_MAX, then that many hashes, none 0), then a byte per entry, bit c
 * set: that player is in crew c of those too; whatever a newer hub adds
 * after them is skipped.  So with a hub no player ever learns another's
 * crews: a crew's hash would be all a stranger near us needs to be linked
 * with us anywhere on the map, and a short crew name is easy to find from it */
#define RC_HF_CREW       0x0200
#define RC_CREW_MAX      4
/* header flags (HELLO, core 8 and up): this player reads the crew marks of
 * the hub's PEERS (RC_HF_CREW above); a hub sends them to no other */
#define RC_HF_HUBCREW    0x0400
/* decoder results */
enum { RC_OK = 0, RC_E_SHORT = -1, RC_E_MAGIC = -2, RC_E_VERSION = -3, RC_E_LENGTH = -4,
       RC_E_CRC = -5, RC_E_TYPE = -6, RC_E_FIELD = -7, RC_E_TRAIL = -8, RC_E_RANGE = -9 };

/* ---- byte helpers -------------------------------------------------------- */
typedef struct { uint8_t *p; uint32_t n, cap; int err; } rc_w;
typedef struct { const uint8_t *p; uint32_t n, len; int err; } rc_r;
void     rc_w_init(rc_w *w, uint8_t *buf, uint32_t cap);
void     rc_put8(rc_w *w, uint32_t v);
void     rc_put16(rc_w *w, uint32_t v);
void     rc_put32(rc_w *w, uint32_t v);
void     rc_putf(rc_w *w, float f);
void     rc_putb(rc_w *w, const void *src, uint32_t n);
void     rc_r_init(rc_r *r, const uint8_t *buf, uint32_t len);
uint32_t rc_get8(rc_r *r);
uint32_t rc_get16(rc_r *r);
uint32_t rc_get32(rc_r *r);
float    rc_getf(rc_r *r);
void     rc_getb(rc_r *r, void *dst, uint32_t n);
uint32_t rc_crc32(const uint8_t *p, uint32_t n);
uint32_t rc_fnv1a(const char *s);           /* 0 for "" */
uint32_t rc_room_hash(const char *s);       /* room names -> room hash: capitals and spaces do not count (0: none) */
uint32_t rc_fnv1a_bytes(const uint8_t *p, uint32_t n);
uint32_t rc_fbits(float f);
float    rc_bitsf(uint32_t u);
float    rc_sqrtf(float x);
/* printable ASCII, at most RC_NAME_MAX bytes, never empty and never one that
 * reads as a slur (names.h): "Player" then */
void     rc_name_clean(char *dst, const char *src, uint32_t srclen);

/* ---- messages -------------------------------------------------------------- */
typedef struct {
    uint32_t t_us;                 /* sender's sync clock (µs, wraps) */
    float    p[3], v[3];           /* m, m/s */
    float    q[4];                 /* body -> world, x y z w */
    float    w[3];                 /* angular velocity, rad/s (world) */
    float    steer;                /* -1 .. 1: the front wheels' angle, 1 = 180 degrees (left positive) */
    float    throttle, brake;      /* 0 .. 1 */
    float    rpm, speed;           /* rpm, m/s (forward) */
    int8_t   gear;
    uint8_t  bits;                 /* RC_BIT_* */
    /* the extension (RC_HF_STATEX): what a peer of core version 4 and up
     * sends, and only when its game side fills it in (ext = 1) */
    uint8_t  ext;
    float    wz[4];                /* the wheel places' heights in the car (m): suspension and hydraulics */
    uint8_t  nos;                  /* nitrous: 0 off, else what is left of it in 1/100 (the game's left / full, up to 2.55) */
    uint8_t  hyd;                  /* each wheel's hydraulics, 2 bits from bit 2 k: the game's state of it (0x5ED470), 0 down */
    uint8_t  thumps;               /* how many times the car landed on its hydraulics (counts on, wraps) */
    uint8_t  slipx;                /* decoded: the slip and surfaces came (a wheel slides) */
    float    slip[8];              /* each wheel's slip, along it and across it (m/s): the game's, car +0x3B4, on the ground only */
    uint8_t  surf[4];              /* each wheel's surface (the game's, car +0x394: 1 road, 8 dirt, 15 grass, ...) */
    uint16_t eng;                  /* the engine as the sender's game sounds it: its rpm on the sound's scale (1000 at idle,
                                      10000 at the most; the clutch and the limiter in it), 0: none */
    uint8_t  eng_acc, eng_dcl, eng_base;   /* ...its voices' volumes, 255 = full: the pulling and the letting-off
                                              recordings (GIN) and the engine model under them (the AEMS bank) */
} rc_state;

typedef struct {
    uint8_t  ver, type;
    uint16_t len;
    uint32_t sender;
    uint16_t seq, flags;
} rc_hdr;

/* a LOBBY message (RC_LOBBY_* op; 24 bytes and 4 per player on the wire) */
typedef struct {
    uint8_t  op, arg, state, max;  /* state RC_LS_*, max: the most players it takes (2 .. RC_LOBBY_MAX) */
    uint32_t to;                   /* the player it is for; 0: the hub */
    uint32_t lobby, race;
    int16_t  x, y;                 /* where the race starts (m) */
    uint16_t t_ms;                 /* START: in this many ms */
    uint8_t  n;                    /* players in it */
    uint8_t  away_s;               /* ADVERT: its host seems away, the lobby closes in this many s (0: not); the byte
                                    * after n, reserved before it: older games send 0 and skip it, as the hub does */
    uint32_t ids[RC_LOBBY_MAX];    /* the host first */
} rc_lobby;

/* a RACEREP (RC_RACEREP_BYTES on the wire): the race (its lobby, the career
 * race's name hash, the network race's type, track and laps), how the
 * reporter did, and its car as the game rated it (mprace.c CarPerf) */
typedef struct {
    uint32_t lobby, race;
    uint8_t  mode;                 /* the network race's type: 0 circuit, 1 sprint, 2 drag, 3 drift, 4 URL, 10 Street X */
    uint8_t  laps;
    uint16_t track;
    uint8_t  place;                /* 1 .. players; 0: not finished */
    uint8_t  players;              /* the race's cars (1 .. RC_LOBBY_MAX) */
    uint8_t  flags;                /* RC_RR_* */
    uint8_t  type;                 /* the car's CarTypeInfo index */
    int8_t   cls;                  /* the game's class: 0 fastest .. 3, -1 not rated */
    float    rating;               /* the game's rating of the car */
    float    field;                /* the average rating of the race's cars */
    float    factor;               /* the car's engine power times this in the race (1: as it is) */
    float    kw, kg, kmh;          /* its engine's most power, its mass, its top speed */
    float    time;                 /* the race's time, s (0: none) */
    char     name[RC_RACEREP_NAME + 1];   /* the car type's name, printable */
} rc_racerep;

/* a RACERES: the race (its lobby and the career race's name hash, the network race's type and laps) and its results'
 * rows in the game's order (1 .. RC_LOBBY_MAX): each racer's player id (0: a racer the reporter could not name),
 * whether it finished and is ours (RC_RS_*), its row's time (s: a finisher's its finish; one still on the way, when
 * the race ended, 30 s after the first), how far it came (laps: the game's count, a finisher as many whole laps as
 * the farthest) and the way it drove (m: its average speed for its time, as the game counts it) */
typedef struct {
    uint32_t lobby, race;
    uint8_t  mode, laps, n;
    struct { uint32_t id; uint8_t flags; float time, done, dist; } row[RC_LOBBY_MAX];
} rc_raceres;

typedef struct {
    rc_hdr h;
    union {
        struct { uint16_t core_ver; uint8_t platform, status; uint16_t listen_port, cfg_rev;
                 uint32_t room; char name[RC_NAME_MAX + 1]; uint8_t n_seen; uint32_t seen[RC_MAX_PEERS];
                 uint8_t n_crews; uint32_t crews[RC_CREW_MAX]; } hello;   /* crews: with RC_HF_CREW only */
        struct { uint8_t n; struct { uint32_t ip; uint16_t port; uint32_t id; } e[RC_MAX_PEERS];
                 uint8_t n_mine; uint32_t mine[RC_CREW_MAX]; uint8_t crew[RC_MAX_PEERS]; } peers;   /* the crew marks: with RC_HF_CREW only */
        struct { uint8_t status, epoch; uint16_t cfg_rev; uint8_t n, clock; rc_state s[RC_SNAP_STATES]; } snap;
        struct { uint16_t cfg_rev; uint8_t frag, nfrag; uint16_t total, dlen; const uint8_t *data; } cfg;
        struct { uint16_t cfg_rev; uint8_t have_mask; } cfgreq;
        struct { uint32_t t0, t1, t2; } ping;          /* PING: t0 only */
        struct { uint8_t reason; } bye;
        struct { uint8_t op, arg; uint32_t to, duel; } duel;      /* to: the player it is for */
        struct { uint16_t req; } mapreq;
        struct { uint16_t req, total; uint8_t part, nparts, n; const uint8_t *data; } map;   /* data: n entries */
        rc_lobby lobby;
        struct { uint16_t req; uint8_t n; const uint8_t *data; } lobbies;   /* data: n entries */
        rc_racerep racerep;
        struct { uint8_t phrase, seq; } say;
        rc_raceres raceres;
    } u;
} rc_msg;

/* Encode one datagram (header, payload, CRC) into out (at least RC_MAX_DGRAM
 * bytes).  Returns its length, or -1 when m does not fit the format. */
int rc_encode(const rc_msg *m, uint8_t *out, int cap);
/* Decode and validate one datagram.  Returns RC_OK or an RC_E_* code; on
 * RC_OK every field is in range (finite floats, positions within ±20 km
 * horizontally and ±2 km vertically, |v| < 200 m/s, a renormalised quaternion,
 * bounded counts, printable names; "Player" for one that reads as a slur,
 * names.h).  m->u.cfg.data points into buf. */
int rc_decode(const uint8_t *buf, int len, rc_msg *m);

/* ---- car-look record (CFG payload, reassembled) ------------------------------ */
typedef struct {
    uint32_t type_hash;                         /* CarTypeInfo +0xD0 */
    char     type_name[RC_CFG_TYPE_NAME + 1];   /* CarTypeInfo +0, for logs and fallbacks */
    uint8_t  ride_b[4];                         /* RideInfo +4..+7 */
    uint8_t  n_parts;
    struct { uint8_t slot; uint32_t key, owner; } part[RC_PART_SLOTS];
    /* extension 1 (52 bytes): what the game's own LAN car description
     * carries for performance, so the receiver builds a complete physics
     * block: 10 tuning flags (RideInfo +0x318, 10 dwords -> 2 bytes of bits),
     * 68 installed performance parts (+0x340, 68 bytes -> 9 bytes of bits),
     * 41 tuning bytes (+0x398) */
    uint8_t  has_perf;
    uint8_t  perf[RC_PERF_BYTES];
    /* extension 2 (1 byte): the horn the player picked (horn.h's kinds; a
     * record without it, from an older core: theirs is the dual horn) */
    uint8_t  has_horn, horn;
} rc_carcfg;
int rc_carcfg_encode(const rc_carcfg *c, uint8_t *out, int cap);   /* bytes, or -1 */
int rc_carcfg_decode(const uint8_t *p, int len, rc_carcfg *c);     /* RC_OK / RC_E_* */

/* ---- interpolation ---------------------------------------------------------- */
typedef struct {
    float   p[3], q[4], v[3], w[3];
    float   steer, throttle, brake, rpm, speed;
    int8_t  gear;
    uint8_t bits;
    uint8_t ext, nos, hyd, thumps; /* as rc_state: ext 0 when the sender sent no extension */
    float   wz[4];
    uint8_t slipx, surf[4];        /* as rc_state: slip 0 when no wheel slides */
    float   slip[8];
    float   eng, eng_acc, eng_dcl, eng_base;   /* as rc_state (the volumes 0 .. 1): eng 0 when the sender sent no engine */
    int     mode;                  /* RC_MODE_* */
} rc_pose;
enum { RC_MODE_NONE = 0, RC_MODE_INTERP = 1, RC_MODE_EXTRAP = 2, RC_MODE_HOLD = 3 };

typedef struct {
    int      n;
    int64_t  t[RC_BUF_STATES];     /* unwrapped sender time, µs */
    rc_state s[RC_BUF_STATES];
    int64_t  t_last;  int t_init;  /* unwrapping */
    uint8_t  epoch;   int epoch_init;   /* teleport epoch: a change snaps */
    uint8_t  clock;   int clock_init;   /* sender clock epoch: a change re-syncs, blended */
    uint8_t  status;
    struct { int64_t at, d; } od[RC_OFFWIN];
    int      odn, odh;
    int64_t  off_r;   int off_init;   /* render offset: host_recv - sender time, slewed */
    int      shown;
    int64_t  prev_rt;
    float    prev_p[3], prev_q[4];    /* target pose shown last frame (without error) */
    float    err_p[3], err_q[4];      /* visual correction, decays */
    int      snap_pending;
    /* statistics */
    uint64_t n_eval, n_interp, n_extrap, n_hold, n_dup, n_old, n_snap, n_epoch;
    uint64_t n_stale;                 /* states of an older (clock) epoch, dropped */
    float    err_max;                 /* largest correction seen (m) */
} rc_interp;

void rc_interp_reset(rc_interp *ip);
/* Insert a received state.  recv_us is the host time of arrival (for the
 * clock offset; pass newest = 1 for the newest state of a SNAP only).
 * Epochs and clock epochs only move forward (mod 256): a state of an older
 * one (a datagram reordered across a teleport or a re-anchor) is dropped. */
void rc_interp_insert(rc_interp *ip, const rc_state *s, uint8_t epoch, uint8_t clock, uint8_t status,
                      int64_t recv_us, int newest);
/* Pose for this frame.  now_us = the receiver's render clock (its sync clock,
 * which follows the local world and the host clock), frame_dt_us = world time
 * since the previous call (0 while the local world is paused).  Returns 1 and
 * fills out when there is anything to show. */
int  rc_interp_eval(rc_interp *ip, int64_t now_us, int64_t frame_dt_us, int64_t delay_us,
                    int64_t extrap_cap_us, rc_pose *out);
/* the same, without touching any state (tests, logs) */
int  rc_interp_peek(const rc_interp *ip, int64_t render_t, int64_t extrap_cap_us, rc_pose *out);
/* exp(-x) for x >= 0 from basic operations only (bit-reproducible) */
float rc_expneg(float x);
/* atan2(y, x) in radians (-pi .. pi) from basic operations only
 * (bit-reproducible; within 3e-7 rad of libm); 0 for (0, 0) */
float rc_atan2f(float y, float x);
/* rotation rows (forward, left, up) <-> quaternion */
void rc_m2q(const float *f, const float *l, const float *u, float *q);
void rc_q2m(const float *q, float *f, float *l, float *u);

/* ---- session ------------------------------------------------------------------ */
typedef struct {
    void *user;
    /* unicast datagram to ip:port (host byte order ip, a.b.c.d = a<<24...) */
    void (*send)(void *user, uint32_t ip, uint16_t port, const uint8_t *buf, int len);
    /* LAN discovery beacon (the platform decides where it may go) */
    void (*broadcast)(void *user, uint16_t port, const uint8_t *buf, int len);
    /* joins, leaves and notes for the log (peer = table index or -1) */
    void (*event)(void *user, int ev, int peer, const char *text);
} rc_io;
enum { RC_EV_JOIN = 1, RC_EV_LEAVE = 2, RC_EV_NOTE = 3, RC_EV_CFG = 4, RC_EV_RESTART = 5 };

typedef struct {
    uint32_t self_id;              /* random, nonzero, per start */
    uint16_t port;                 /* our listen port (HELLO) */
    uint32_t room;                 /* rc_room_hash(room name) */
    uint32_t crews[RC_CREW_MAX];   /* rc_room_hash(crew name), each its own, in the player's order (n_crews of them) */
    int      n_crews;
    char     name[RC_NAME_MAX + 1];
    uint8_t  platform;
    int      discovery;            /* beacon through io.broadcast every 2 s */
    int      test_mode;            /* only 127/8 peers are accepted */
    int      max_players;          /* 2 .. RC_MAX_PLAYERS */
    int      rate_hz;              /* snapshots per second (10 .. 60) */
    /* states per SNAP, newest first (1 .. RC_SNAP_STATES; 0 = all): the
     * older ones stand in for a SNAP lost on the way.  A link that loses
     * none (the browser's WebSocket to the server's switch) sends 1 */
    int      snap_states;
    /* a free-roam hub (the browser's server, web/server-rs/crates/roam), or 0:
     * it gets a HELLO every second and a SNAP twice a second, and answers
     * every half second with a PEERS list of the players near us.  While it
     * answers, that list is who our peers are: nobody else is taken, a peer
     * it no longer lists goes ("out of range"), and there is no discovery
     * and no gossip.  Silent for 10 s (or from the start): discovery and
     * gossip as without one. */
    uint32_t hub_ip;
    uint16_t hub_port;
    int      duels;                /* we take outrun challenges (HELLO says so: RC_HF_DUELS) */
    int      lobbies;              /* we take race lobbies (HELLO says so: RC_HF_LOBBY) */
    int      say;                  /* we take quick chat (HELLO says so: RC_HF_SAY) */
} rc_config;

enum { RC_PS_FREE = 0, RC_PS_CANDIDATE = 1, RC_PS_HEARD = 2, RC_PS_EST = 3 };
typedef struct {
    int      state;
    int      configured;           /* from the static list: never forgotten */
    int      gossiped;
    uint32_t ip;  uint16_t port;
    uint32_t id;
    int64_t  added_us, last_rx_us, last_hello_tx, last_ping_tx, last_cfgreq_tx, last_snap_rx;
    int64_t  cfg_answer_us;        /* our record goes to it again (asked for, or at its join) from then on */
    int64_t  hub_listed_us;        /* the hub's list named it last (0: never) */
    char     name[RC_NAME_MAX + 1];
    uint8_t  platform, status;
    uint16_t core_ver;             /* its HELLO's (0 until one came): below 2 it reads lists of RC_PEERS_V1 at most */
    uint8_t  duels;                /* its HELLO says it takes outrun challenges (RC_HF_DUELS) */
    uint8_t  lobbies;              /* its HELLO says it takes race lobbies (RC_HF_LOBBY) */
    uint8_t  statex;              /* its HELLO says it reads state extensions (RC_HF_STATEX) */
    uint8_t  say;                  /* its HELLO says it takes quick chat (RC_HF_SAY) */
    uint8_t  reads_crews;          /* its HELLO says it reads crews (RC_HF_CREW) */
    uint8_t  n_crews;  uint32_t crews[RC_CREW_MAX];   /* its crews, as its last HELLO had them (an older core's, or no hub) */
    uint8_t  n_hub_crews; uint32_t hub_crews[RC_CREW_MAX];   /* our crews it is in too, as the hub's last list naming it said */
    uint16_t cfg_rev_seen;         /* what the peer announces */
    int      hears_us, lost;
    int      hears_miss;           /* with the hub: one HELLO without our id forgiven (hub mode only) */
    /* rate limit */
    int64_t  rl_t0;  int rl_n, rl_bytes;
    /* statistics; seq_lost counts gaps in the sender's snapshot sequence */
    uint32_t rx, rx_snap, rx_drop, seq_lost;  uint16_t last_seq;  int seq_init;
    int32_t  rtt_us;
    /* car look */
    uint16_t cfg_bad_rev;          /* a record this core cannot read: not asked for again */
    uint16_t cfg_rev;              /* complete record held */
    int      cfg_len;  uint8_t cfg[RC_CFG_MAX];
    uint32_t cfg_gen;              /* +1 for every new complete record */
    uint16_t asm_rev;  uint16_t asm_total;  uint8_t asm_mask, asm_nfrag;  uint8_t asm_buf[RC_CFG_MAX];
    rc_interp mo;                  /* motion: jitter buffer and smoothing */
} rc_peer;

typedef struct {
    rc_config conf;
    rc_io     io;
    rc_peer   peer[RC_TABLE];
    uint16_t  seq;                 /* snapshot sequence: +1 per SNAP, carried by every datagram */
    uint8_t   status, epoch, clock;
    /* local car look */
    uint16_t  cfg_rev;  int cfg_len;  uint8_t cfg[RC_CFG_MAX];
    /* local snapshots */
    rc_state  last[RC_SNAP_STATES];  int nlast;
    int64_t   next_snap_us;
    int64_t   last_beacon, last_peers_tx;  int peers_dirty;
    /* statistics */
    uint32_t  tx, tx_bytes, rx, rx_bad, rx_policy, rx_ratelimit, rx_unknown_full, rx_version;
    uint32_t  rx_room;             /* HELLOs of another room */
    uint32_t  rx_dup_id;           /* a known player's id from a second address: ignored */
    uint32_t  rx_stray;            /* another id from a known address, not a HELLO: ignored */
    int64_t   note_version_us, note_room_us;  int noted_version, noted_room;   /* mismatch notes, every 30 s */
    /* after rc_shutdown nothing more is sent; a player's datagrams that
     * arrive after its BYE (sent before it, or overtaking it) are ignored
     * for 3 s, so it does not join again as a ghost */
    int       closed;
    struct { uint32_t id; int64_t at; } bye[4];  int nbye;
    uint32_t  rx_after_bye;
    /* the hub (conf.hub_ip): the first tick, its last list, our last HELLO
     * and SNAP to it, whether it is the one that decides now (logged when
     * that changes) */
    int64_t   hub_since, hub_heard_us, hub_hello_tx, hub_snap_tx;  int hub_was;
    uint32_t  hub_lists;           /* PEERS lists from the hub */
    uint32_t  rx_unlisted;         /* HELLOs of players the hub does not list for us: ignored */
    /* DUEL messages for us from established peers, oldest first, until the
     * game side takes them (rc_take_duel) */
    struct { uint32_t from; uint8_t op, arg; uint32_t duel; } duel_in[RC_DUEL_INBOX];
    int       n_duel_in;
    uint32_t  duel_dropped;        /* the inbox was full */
    /* the world map's other players (rc_ask_map): our last question, the
     * answer's parts so far, and the last complete answer (map_gen +1 for
     * each); entry k of part j at j * RC_MAP_PER + k */
    uint16_t  map_req;  int64_t map_asked_us;
    uint8_t   map_mask, map_nparts;  uint8_t map_part_n[RC_MAP_PARTS];  uint16_t map_asm_total;
    uint8_t   map_asm[RC_MAP_PARTS * RC_MAP_PER * RC_MAP_ENTRY];
    uint32_t  map_gen;  int map_n;  uint16_t map_total;
    struct { int16_t x, y; uint8_t heading; } map_e[RC_MAP_PARTS * RC_MAP_PER];
    /* LOBBY messages for us (to our id) from established peers, oldest
     * first, until the game side takes them (rc_take_lobby) */
    struct { uint32_t from; rc_lobby l; } lobby_in[RC_LOBBY_INBOX];
    int       n_lobby_in;
    uint32_t  lobby_dropped;       /* the inbox was full */
    /* the hub's open lobbies of our room, with the answer to our last map
     * question (lobbies_gen +1 for each) */
    uint32_t  lobbies_gen;  int lobbies_n;  uint16_t lobbies_req;   /* (the question they answered) */
    struct { int16_t x, y; uint32_t race; uint8_t n, max, state; } lobbies_e[RC_LOBBIES_MAX];
    /* SAY messages from established peers, oldest first, until the game
     * side takes them (rc_take_say) */
    struct { uint32_t from; uint8_t phrase, seq; } say_in[RC_SAY_INBOX];
    int       n_say_in;
    uint32_t  say_dropped;         /* the inbox was full */
} rc_session;

void rc_session_init(rc_session *s, const rc_config *cfg, const rc_io *io);
/* a configured peer (static list) or a gossiped one; returns its index or -1 */
int  rc_add_peer(rc_session *s, uint32_t ip, uint16_t port, int configured, int64_t now_us);
int  rc_addr_private(uint32_t ip);       /* RFC 1918, link-local 169.254/16, loopback 127/8 */
void rc_receive(rc_session *s, uint32_t ip, uint16_t port, const uint8_t *buf, int len, int64_t now_us);
void rc_tick(rc_session *s, int64_t now_us);
void rc_set_status(rc_session *s, uint8_t status);
/* the local car-look record; a change bumps cfg_rev and pushes it */
void rc_set_cfg(rc_session *s, const uint8_t *rec, int len, int64_t now_us);
/* the local car state, every frame: a SNAP goes out when one is due
 * (rate_hz, host clock), with the last snap_states states */
int  rc_push_state(rc_session *s, const rc_state *st, int64_t now_us);
void rc_bump_epoch(rc_session *s);       /* teleport: receivers snap */
void rc_bump_clock(rc_session *s);       /* stamps re-anchored: receivers re-sync, blended */
void rc_shutdown(rc_session *s, uint8_t reason);   /* BYE x3 to every peer, then the session is silent */
int  rc_established(const rc_session *s);
/* peer i is on the hub's last list (or there is no hub): a peer left out goes within T_HUB_GRACE, and the game gives
 * its car's slot to one the hub lists */
int  rc_peer_listed(const rc_session *s, int i);
/* the first of our crews (conf.crews) that peer i is in too: 0 .. n_crews - 1, or -1 (as the hub's list marks it,
 * or as its HELLO names its crews: an older core's, or without a hub) */
int  rc_peer_crew(const rc_session *s, int i);
/* our crews changed while the session runs: at most RC_CREW_MAX, 0s and
 * repeats left out; the hub (or without one, the peers) hears them at the
 * next tick */
void rc_set_crews(rc_session *s, const uint32_t *crews, int n);
/* a DUEL message to the established peer with id `to`, if its HELLO said it
 * takes them; 1 when sent */
int  rc_send_duel(rc_session *s, uint32_t to, uint8_t op, uint8_t arg, uint32_t duel);
/* the oldest DUEL message for us not taken yet; 1 when there was one */
int  rc_take_duel(rc_session *s, uint32_t *from, uint8_t *op, uint8_t *arg, uint32_t *duel);
/* asks the hub (while it answers) for the world map's other players; 1 when
 * asked.  The answer is in map_e[0 .. map_n) once map_gen changes */
int  rc_ask_map(rc_session *s, int64_t now_us);
/* a LOBBY message to the established peer with id l->to, if its HELLO said
 * it takes them, or (to 0) to the hub while it answers; 1 when sent */
int  rc_send_lobby(rc_session *s, const rc_lobby *l);
/* the oldest LOBBY message for us not taken yet; 1 when there was one */
int  rc_take_lobby(rc_session *s, uint32_t *from, rc_lobby *l);
/* a race report to the hub (RC_MSG_RACEREP), while it answers; 1 when sent */
int  rc_send_racerep(rc_session *s, const rc_racerep *r);
/* the race's results to the hub (RC_MSG_RACERES), while it answers; 1 when sent */
int  rc_send_raceres(rc_session *s, const rc_raceres *r);
/* a quick-chat phrase to every established peer whose HELLO said it takes
 * them; how many it went to */
int  rc_send_say(rc_session *s, uint8_t phrase, uint8_t seq);
/* the oldest SAY not taken yet; 1 when there was one */
int  rc_take_say(rc_session *s, uint32_t *from, uint8_t *phrase, uint8_t *seq);
const char *rc_status_name(int st);

#endif
