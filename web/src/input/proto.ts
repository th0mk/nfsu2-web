// The HostMailbox layout: the page side of runtime/web/input_proto.h (same
// names, same numbers; web/tools/check-proto.mjs compares the two files).

export const INPUT_MAGIC = 0x4E494E50;
export const INPUT_VERSION = 1;
export const INPUT_EV_N = 1024;
export const INPUT_SYN_N = 256;
export const INPUT_NAME_LEN = 16;
export const INPUT_TEXT_BYTES = 96;

// header words
export const IM_MAGIC = 0;
export const IM_VERSION = 1;
export const IM_SIZE = 2;
export const IM_EV_N = 3;
export const IM_EV_W = 4;           // page: events written (notify after writing)
export const IM_EV_R = 5;           // wasm: events consumed
export const IM_EV_DROP = 6;        // page
export const IM_ATTACHED = 7;       // page 1 / wasm 2 (no page input: ns* input translated in C), compare-exchange from 0
export const IM_SYN_W = 8;          // wasm: synthetic requests written
export const IM_SYN_R = 9;          // page: synthetic requests consumed
export const IM_SYN_DROP = 10;      // wasm
export const IM_PAD_SEQ = 11;       // page: seqlock over the pad words (odd while writing)
export const IM_PAD_CONNECTED = 12;
export const IM_PAD_FAMILY = 13;
export const IM_PAD_BUTTONS = 14;
export const IM_PAD_GEN = 15;
export const IM_PAD_AXIS = 16;      // 16..21, float bits
export const IM_NAMES_GEN = 22;     // page
export const IM_MOUSE_X = 23;       // wasm
export const IM_MOUSE_Y = 24;       // wasm
export const IM_KEYS_HELD = 25;     // wasm
export const IM_EVENTS = 26;        // wasm
export const IM_GAME_INACTIVE = 27; // wasm
export const IM_QUIT_REQ = 28;      // wasm
export const IM_RUMBLE_SEQ = 29;    // wasm: bumped with every rumble update
export const IM_RUMBLE_STRONG = 30; // wasm: 0..65535, the low-frequency motor
export const IM_RUMBLE_WEAK = 31;   // wasm: 0..65535, the high-frequency motor
export const IM_PAD_RUMBLE = 32;    // page: 1 when the controller driving the pad can rumble
export const IM_TEXT_SEQ = 33;      // wasm: seqlock over the text entry words (odd while writing)
export const IM_TEXT_MAX = 34;      // wasm: the game's text entry is up: the most characters it takes (0: none)
export const IM_TEXT_KIND = 35;     // wasm: which characters it takes besides letters and digits
export const IM_TEXT_LEN = 36;      // wasm: the characters in it now
export const IM_PAD_WHEEL = 37;     // page: 1 when the controller driving the pad is a steering wheel (under IM_PAD_SEQ)
export const IM_VIEW_ROLL = 38;     // page: float bits, the phone's tilt the 3D view turns against (radians, + = clockwise)
export const IM_MENU_SCALE = 39;    // page: float bits, the menus' help bar buttons' and arrows' size (1 = the game's own;
                                    // 0 = not set; negative: the arrows' alone, the bar at the game's own)
export const IM_TEXT = 40;          // wasm: 40..63, its text (cp1252, NUL-padded)
export const IM_HDR_WORDS = 64;

// events, page -> wasm
export const IEV_KEY_DOWN = 1;
export const IEV_KEY_UP = 2;
export const IEV_KEY_TAP = 3;
export const IEV_MOUSE_MOVE = 4;
export const IEV_MOUSE_DOWN = 5;
export const IEV_MOUSE_UP = 6;
export const IEV_WHEEL = 7;
export const IEV_FOCUS = 8;
export const IEV_RELEASE_ALL = 9;
export const IEV_QUIT = 10;
export const IEV_TEXT = 11;
export const IEV_LOG = 12;
export const IEV_SMS = 13;
export const IEV_PHOTO = 14;

export const IKF_REPEAT = 1;
export const IKF_ALT = 2;
export const IKF_CTRL = 4;
export const IKF_SHIFT = 8;
export const IKF_DEAD = 16;
export const IKF_SYNTH = 32;
export const IKF_TOUCH = 64;        // IEV_MOUSE_MOVE: a finger landed there (a tap's start)

export const IREASON_NONE = 0;
export const IREASON_WINDOW = 1;
export const IREASON_VISIBILITY = 2;
export const IREASON_FULLSCREEN = 3;
export const IREASON_META = 4;
export const IREASON_PAGEHIDE = 5;
export const IREASON_SCRIPT = 6;
export const IREASON_ELEMENT = 7;
export const IREASON_PAGE = 8;

// synthetic requests, wasm -> page (types: runtime/kbd.h KSYN_*)
export const ISYN_SHIFT = 1;
export const ISYN_CTRL = 2;
export const ISYN_ALT = 4;
export const ISYN_META = 8;
export const KSYN_DOWN = 0;
export const KSYN_UP = 1;
export const KSYN_REPEAT = 2;
export const KSYN_CHAR_DOWN = 3;
export const KSYN_CHAR_UP = 4;
export const KSYN_FOCUS_OUT = 5;
export const KSYN_FOCUS_IN = 6;
export const KSYN_MOUSE_MOVE = 7;
export const KSYN_MOUSE_DOWN = 8;
export const KSYN_MOUSE_UP = 9;
export const KSYN_SCROLL = 10;

// byte offsets
export const IM_OFF_PAD_NAME = 256;
export const IM_OFF_NAMES = 320;
export const IM_OFF_EV = 4416;
export const IM_OFF_SYN = 20800;
export const IM_BYTES = 24896;

// runtime/gamepad.h
export const PAD_A = 0, PAD_B = 1, PAD_X = 2, PAD_Y = 3, PAD_LB = 4, PAD_RB = 5, PAD_BACK = 6, PAD_START = 7,
  PAD_LS = 8, PAD_RS = 9, PAD_HOME = 10, PAD_DUP = 11, PAD_DDOWN = 12, PAD_DLEFT = 13, PAD_DRIGHT = 14;
export const PAD_AXIS_LX = 0, PAD_AXIS_LY = 1, PAD_AXIS_RX = 2, PAD_AXIS_RY = 3, PAD_AXIS_LT = 4, PAD_AXIS_RT = 5;
export const PADF_GENERIC = 0, PADF_XBOX = 1, PADF_PLAYSTATION = 2, PADF_NINTENDO = 3;
