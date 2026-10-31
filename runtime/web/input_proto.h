/* The HostMailbox: input from the page to the game, registered with
 * nfsu2_register(HOST_KIND_INPUT, mb, sizeof *mb) by runtime/web/input_web.c.
 *
 * Guest threads never return to their worker's event loop, so nothing can
 * be posted to them: everything the page has for the game goes through this
 * shared structure, and every field has exactly one writer.
 *   page -> wasm   an event ring (keys, text, mouse, focus, quit), the game
 *                  pad state under a seqlock, the key names of the current layout
 *   wasm -> page   a ring of synthetic input requests (the autotest ns*
 *                  commands, which the page turns into DOM events so they take
 *                  the same path as real input), a few status words, and the
 *                  game's text entry under a seqlock
 * The header is an array of 32-bit words mirrored by index in
 * web/src/input/proto.ts; web/tools/check-proto.mjs compares the two files. */
#ifndef NFSU2_INPUT_PROTO_H
#define NFSU2_INPUT_PROTO_H
#include <stdint.h>
#include <stddef.h>

#define INPUT_MAGIC    0x4E494E50u  /* 'NINP' */
#define INPUT_VERSION  1u
#define INPUT_EV_N     1024u        /* power of two */
#define INPUT_SYN_N    256u         /* power of two */
#define INPUT_NAME_LEN 16u          /* bytes per key name (cp1252, NUL-terminated) */
#define INPUT_TEXT_BYTES 96u        /* header words IM_TEXT..63: the game's text entry */

/* header words */
enum {
    IM_MAGIC = 0,
    IM_VERSION = 1,
    IM_SIZE = 2,          /* sizeof(HostMailbox) */
    IM_EV_N = 3,
    IM_EV_W = 4,          /* page: events written (the input thread waits on this word) */
    IM_EV_R = 5,          /* wasm: events consumed */
    IM_EV_DROP = 6,       /* page: events dropped, ring full */
    IM_ATTACHED = 7,      /* who writes the event ring: 0 nobody, 1 the page (attached; back to 0
                             after its detach), 2 wasm (no page input layer: the autotest's ns*
                             input is translated in input_web.c); claimed by compare-exchange from 0 */
    IM_SYN_W = 8,         /* wasm: synthetic requests written */
    IM_SYN_R = 9,         /* page: synthetic requests consumed */
    IM_SYN_DROP = 10,     /* wasm: requests dropped, ring full */
    IM_PAD_SEQ = 11,      /* page: seqlock of the pad words below (odd while writing) */
    IM_PAD_CONNECTED = 12,
    IM_PAD_FAMILY = 13,   /* PADF_* (gamepad.h) */
    IM_PAD_BUTTONS = 14,  /* 1u << PAD_* */
    IM_PAD_GEN = 15,      /* bumped when the controller driving the pad changes */
    IM_PAD_AXIS = 16,     /* 16..21: float bits, PAD_AXIS_LX .. PAD_AXIS_RT */
    IM_NAMES_GEN = 22,    /* page: bumped after the key names were (re)written */
    IM_MOUSE_X = 23,      /* wasm: cursor in game client coordinates */
    IM_MOUSE_Y = 24,
    IM_KEYS_HELD = 25,    /* wasm: keys the game holds */
    IM_EVENTS = 26,       /* wasm: events handled */
    IM_GAME_INACTIVE = 27,/* wasm: 1 while the game is parked (told it is inactive, no LAN session) */
    IM_QUIT_REQ = 28,     /* wasm: quit requests seen */
    IM_RUMBLE_SEQ = 29,   /* wasm: bumped with every rumble update (the page stops the motors when it stands) */
    IM_RUMBLE_STRONG = 30,/* wasm: 0..65535, the low-frequency motor */
    IM_RUMBLE_WEAK = 31,  /* wasm: 0..65535, the high-frequency motor */
    IM_PAD_RUMBLE = 32,   /* page: 1 when the controller driving the pad can rumble */
    IM_TEXT_SEQ = 33,     /* wasm: seqlock of the text entry words below (odd while writing) */
    IM_TEXT_MAX = 34,     /* wasm: the game's text entry is up: the most characters it takes (0: none is up) */
    IM_TEXT_KIND = 35,    /* wasm: its kind, which characters it takes besides letters and digits */
    IM_TEXT_LEN = 36,     /* wasm: the characters in it now */
    IM_PAD_WHEEL = 37,    /* page: 1 when the controller driving the pad is a steering wheel (under IM_PAD_SEQ) */
    IM_VIEW_ROLL = 38,    /* page: float bits, the phone's tilt the 3D view turns against (radians, + = the phone turned
                             clockwise; 0 = the game's view), the touch controls' level horizon */
    IM_MENU_SCALE = 39,   /* page: float bits, the menus' help bar buttons' and scroller arrows' size (1 = the game's
                             own; 0 = not set; negative: the arrows' alone, the bar at the game's own), the Controls
                             sheet's Menu button size (runtime/menusize.c) */
    IM_TEXT = 40,        /* wasm: 40..63, its text (cp1252, NUL-padded, the first INPUT_TEXT_BYTES) */
    IM_HDR_WORDS = 64
};

/* page -> wasm events */
enum {
    IEV_KEY_DOWN = 1,     /* dik, vk, flags, chars; b = the key's plain character (WM_SYSCHAR) */
    IEV_KEY_UP = 2,
    IEV_KEY_TAP = 3,      /* down + up at once (Caps Lock on macOS: one event per toggle) */
    IEV_MOUSE_MOVE = 4,   /* a, b = position in the shown image, 1/65536 of its size (may be outside) */
    IEV_MOUSE_DOWN = 5,   /* a = DirectInput button (0 left, 1 right, 2 middle, 3, 4) */
    IEV_MOUSE_UP = 6,
    IEV_WHEEL = 7,        /* a = delta in WHEEL_DELTA units (120 per notch, + = away from the user) */
    IEV_FOCUS = 8,        /* a = 1 active / 0 inactive, b = IREASON_* */
    IEV_RELEASE_ALL = 9,  /* b = IREASON_*: every key and button the game holds goes up */
    IEV_QUIT = 10,        /* the player asked to quit: WM_CLOSE */
    IEV_TEXT = 11,        /* chars typed with no key (a phone's keyboard): WM_CHAR each, 0x08 deletes, 0x0D accepts */
    IEV_LOG = 12,         /* Download log: how long ago the last frame was, a stall report if long (watchdog.c) */
    IEV_SMS = 13,         /* a = a text message of ours for the game's phone (sms.c) */
    IEV_PHOTO = 14,       /* photo mode (photo.c): dik = what (PH_*), a, b its values */
};
enum {                    /* event flags */
    IKF_REPEAT = 1,       /* autorepeat */
    IKF_ALT = 2,          /* Alt / Option held */
    IKF_CTRL = 4,         /* Control held */
    IKF_SHIFT = 8,        /* Shift held */
    IKF_DEAD = 16,        /* a dead key: ch[0] = its spacing character */
    IKF_SYNTH = 32,       /* a synthetic DOM event (autotest), for the log */
    IKF_TOUCH = 64,       /* IEV_MOUSE_MOVE: a finger landed there (a tap's start) */
};
enum {                    /* why the page sent a focus change or a release */
    IREASON_NONE = 0, IREASON_WINDOW = 1, IREASON_VISIBILITY = 2, IREASON_FULLSCREEN = 3,
    IREASON_META = 4, IREASON_PAGEHIDE = 5, IREASON_SCRIPT = 6, IREASON_ELEMENT = 7, IREASON_PAGE = 8,
};

typedef struct {
    uint8_t type, dik, vk, flags;
    uint8_t nch, ch[3];           /* WM_CHAR bytes, cp1252 */
    int32_t a, b;
} InEv;

/* wasm -> page synthetic input (kbd.h KSYN_*) */
enum { ISYN_SHIFT = 1, ISYN_CTRL = 2, ISYN_ALT = 4, ISYN_META = 8 };
typedef struct {
    uint8_t type, dik, mods, pad; /* KSYN_*, the key, ISYN_* modifiers to hold around it */
    uint32_t ch;                  /* KSYN_CHAR_*: the character (Unicode) */
    float a, b;                   /* mouse: position (fractions) / button; scroll: lines */
} SynReq;

typedef struct {
    _Atomic uint32_t h[IM_HDR_WORDS];
    char    pad_name[64];                         /* page: under IM_PAD_SEQ */
    char    names[256][INPUT_NAME_LEN];           /* page: key names by DIK, "" = none */
    InEv    ev[INPUT_EV_N];
    SynReq  syn[INPUT_SYN_N];
} HostMailbox;

#define IM_OFF_PAD_NAME 256
#define IM_OFF_NAMES    320
#define IM_OFF_EV       4416
#define IM_OFF_SYN      20800
#define IM_BYTES        24896
_Static_assert(sizeof(InEv) == 16 && sizeof(SynReq) == 16, "input record sizes");
_Static_assert(offsetof(HostMailbox, pad_name) == IM_OFF_PAD_NAME, "mailbox layout");
_Static_assert(offsetof(HostMailbox, names) == IM_OFF_NAMES, "mailbox layout");
_Static_assert(offsetof(HostMailbox, ev) == IM_OFF_EV, "mailbox layout");
_Static_assert(offsetof(HostMailbox, syn) == IM_OFF_SYN, "mailbox layout");
_Static_assert(sizeof(HostMailbox) == IM_BYTES, "mailbox layout");
_Static_assert(IM_TEXT + INPUT_TEXT_BYTES / 4 <= IM_HDR_WORDS, "the text entry fits the header");
#endif
