/* dinput_pad.c -- the virtual game pad (runtime/gamepad.h: a browser
 * controller, the Mac's GameController.framework, or the test script's) as a
 * DirectInput 8 joystick device, the same for every game.
 *
 * The device is a classic DIJOYSTATE2 gamepad (c_dfDIJoystick2, 0x110 bytes;
 * DIJOYSTATE's 0x50 too) with a STABLE guidInstance: a game keys the
 * bindings it saves on it.  Layout:
 *   axes    X/Y = left stick, Z = left trigger, Rx/Ry = right stick,
 *           Rz = right trigger (triggers rest at the minimum)
 *   POV 0   D-pad;  POV 1  left stick as four directions (menus)
 *   buttons 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 View, 7 Menu, 8 LS, 9 RS,
 *           10 LT and 11 RT pressed to the end of their travel (as a
 *           DualShock 4 under DirectInput reports L2/R2 twice), or half
 *           way when the game reads that trigger as no axis (fill_state,
 *           game.h pad_trigger_axes); or in the order the game's own
 *           layout expects (game.h pad_buttons: Most Wanted's is a
 *           DualShock's, Cross = button 1)
 *
 * A steering wheel (PadState.wheel: the page maps the wheel to LX, the gas
 * and brake pedals to RT and LT, its buttons onto the pad's, see
 * web/src/input/wheel.ts) is offered as a driving device instead, with an
 * instance GUID of its own, so a game keeps its bindings apart from the pad's.
 *
 * The device is offered only once a controller exists (LESSONS L-105): with
 * none, EnumDevices offers no game controller, as on Windows, so a keyboard
 * player never meets a pad in the game's controller screens.  A controller
 * connected later reaches the game through its own search where it has one
 * (game.h pad_rescan: UG2 enumerates again every 32 frames while a flag is
 * set), or else through the callback of the game's enumeration that found
 * none, where the game's file names that callback (game.h pad_late:
 * Most Wanted enumerates once, dipad_present).  A game with a search of its
 * own also gets a device of the other kind when a wheel takes over from a pad
 * or back: the old one is let go as in hotplug mode (two polls at rest, then
 * DIERR_INPUTLOST) and the search finds the new one.  Without a search the
 * device stays and reads whichever controller drives it.
 *
 *   NFSU2_PAD=0         no game pad device (also off/no/false)
 *   NFSU2_PAD=hotplug   the device exists only while a controller is
 *                       connected (DirectInput-faithful: unplugging reports
 *                       DIERR_INPUTLOST, and the game's search stays on).
 *                       Default: the device appears once a controller has
 *                       been connected (at start-up or later) and then
 *                       stays, reading centred while nothing is connected,
 *                       so bindings survive unplugging.
 *   NFSU2_PAD_TRIGGER_CLICK=<f>  trigger travel (0..1) at which buttons 10/11
 *                       press while the game reads the trigger as an axis
 *                       (default 0.97)
 *   NFSU2_PAD_LOG=1     log enumeration, properties and state changes
 *
 * Force feedback (rumble): when the platform can drive the controller's
 * motors (gamepad_rumble_ok: the browser's dual-rumble; the Mac's gamepad.m
 * has no motors and keeps the old answer), the pad reports force feedback
 * and plays the game's effects itself: every frame the running ones are mixed
 * into two motor levels (see ff_mix), with the jolts the game's file measures
 * on top (game.h pad_jolt: UG2's player car, as the console versions rumble).
 *   NFSU2_FF=0          no force feedback and no rumble (the pad as before;
 *                       settings.ini "rumble = off")
 *   NFSU2_FF_LOG=1      log the effects the game creates, changes, starts and
 *                       stops, and the motor levels when they change
 *
 * What one game needs beyond this (its own profile lookup and Joypad page,
 * its input channels, its search flag, its front-end keystrokes) is in that
 * game's files, reached through runtime/game.h: runtime/pad_ug2.c for UG2,
 * runtime/mw/game_mw.c's table for Most Wanted.  A core file: no game
 * address here (tools/gamelit.sh, runtime/core.list). */
#include "cpu.h"
#include "game.h"
#include "gamepad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <time.h>

uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
uint32_t com_scratch(uint32_t n);

#define DI_OK                 0x00000000u
#define DI_NOEFFECT           0x00000001u
#define DIERR_OBJECTNOTFOUND  0x80070002u
#define DIERR_INPUTLOST       0x8007001Eu
#define DIERR_INVALIDPARAM    0x80070057u
#define DIERR_UNSUPPORTED     0x80004001u

#define DIDFT_ABSAXIS   0x00000002u
#define DIDFT_PSHBUTTON 0x00000004u
#define DIDFT_POV       0x00000010u
#define DIDFT_TYPEMASK  0x000000FFu
#define DIDOI_ASPECTPOSITION 0x00000100u
#define DIDOI_FFACTUATOR     0x00000001u
#define DIDFT_FFACTUATOR     0x01000000u
#define DIDC_FORCEFEEDBACK   0x00000100u
#define DI8DEVTYPE_GAMEPAD 0x15u
#define DI8DEVTYPE_DRIVING 0x16u
#define PAD_DEVTYPE     (0x00010000u | (2u << 8) | DI8DEVTYPE_GAMEPAD)  /* HID, STANDARD */
#define WHEEL_DEVTYPE   (0x00010000u | (3u << 8) | DI8DEVTYPE_DRIVING)  /* HID, DUALPEDALS */

/* ---- configuration ---------------------------------------------------- */
enum { MODE_OFF, MODE_ALWAYS, MODE_HOTPLUG };
static int pad_mode(void){
    static int m = -1;
    if (m < 0){
        const char *e = getenv("NFSU2_PAD");
        m = e && !strcasecmp(e, "hotplug") ? MODE_HOTPLUG
          : gamepad_env_flag("NFSU2_PAD", 1) ? MODE_ALWAYS : MODE_OFF;
        if (m != MODE_OFF) gamepad_init();
    }
    return m;
}
static int pad_log(void){
    static int l = -1;
    if (l < 0) l = gamepad_env_flag("NFSU2_PAD_LOG", 0);
    return l;
}
/* for the games' pad files (pad_ug2.c, mw/pad_mw.c) */
int dipad_on(void){ return pad_mode() != MODE_OFF; }
int dipad_log(void){ return pad_log(); }
/* the game's own search for controllers (game.h pad_rescan), on or off; nothing for a game without one */
static void pad_rescan(int on){ if (g_game.pad_rescan) g_game.pad_rescan(on); }

/* ---- objects ---------------------------------------------------------------
 * Axes 0..5, POVs 6..7, buttons 8..19.  Object GUIDs are dinput.h's
 * GUID_XAxis family: {A36D02xx-C9F3-11CF-BFC7-444553540000}. */
#define NAXES 6
#define NPOVS 2
#define NBTNS 12
#define NOBJS (NAXES + NPOVS + NBTNS)
static const uint8_t k_axis_ofs[NAXES]  = {0x00, 0x04, 0x08, 0x0C, 0x10, 0x14};
static const uint32_t k_axis_guid[NAXES] = {0xA36D02E0u, 0xA36D02E1u, 0xA36D02E2u,
                                            0xA36D02F4u, 0xA36D02F5u, 0xA36D02E3u};
static const uint16_t k_axis_usage[NAXES] = {0x30, 0x31, 0x32, 0x33, 0x34, 0x35};
/* which pad axis feeds each DirectInput axis */
static const int k_axis_src[NAXES] = {PAD_AXIS_LX, PAD_AXIS_LY, PAD_AXIS_LT,
                                      PAD_AXIS_RX, PAD_AXIS_RY, PAD_AXIS_RT};
#define SRC_LT_FULL -1
#define SRC_RT_FULL -2
static const int k_btn_src[NBTNS] = {PAD_A, PAD_B, PAD_X, PAD_Y, PAD_LB, PAD_RB,
                                     PAD_BACK, PAD_START, PAD_LS, PAD_RS,
                                     SRC_LT_FULL, SRC_RT_FULL};
/* The pad button behind each DirectInput button: k_btn_src, or the game's own
 * order (game.h pad_buttons: the names of NBTNS buttons, LT and RT the
 * triggers pulled to the end), read once.  An order that does not name
 * NBTNS buttons is refused with a line, and the default stays. */
static const int *btn_src(void){
    static int order[NBTNS], done;
    if (done) return order;
    done = 1;
    memcpy(order, k_btn_src, sizeof order);
    const char *p = g_game.pad_buttons;
    if (!p) return order;
    int got[NBTNS], n = 0;
    while (*p){
        size_t l = strspn(p, " "); p += l;
        if (!*p) break;
        l = strcspn(p, " ");
        char tok[12];
        int s = -100;
        if (l < sizeof tok && n < NBTNS){
            memcpy(tok, p, l); tok[l] = 0;
            s = !strcasecmp(tok, "LT") ? SRC_LT_FULL : !strcasecmp(tok, "RT") ? SRC_RT_FULL : gamepad_button_by_name(tok);
            if (s == -1 && strcasecmp(tok, "LT")) s = -100;     /* no such button */
        }
        if (s == -100){ n = -1; break; }
        got[n++] = s; p += l;
    }
    if (n == NBTNS) memcpy(order, got, sizeof order);
    else fprintf(stderr, "[pad] the game's button order \"%s\" does not name %d buttons: the pad's own order instead\n",
                 g_game.pad_buttons, NBTNS);
    return order;
}
/* the default position of DirectInput button i's pad button, for its name (-1: none there) */
static int btn_name_slot(int i){
    int src = btn_src()[i];
    for (int j = 0; j < NBTNS; j++) if (k_btn_src[j] == src) return j;
    return -1;
}

static uint32_t obj_ofs(int i){
    return i < NAXES ? k_axis_ofs[i] : i < NAXES + NPOVS ? 0x20u + 4u * (uint32_t)(i - NAXES)
                                                       : 0x30u + (uint32_t)(i - NAXES - NPOVS);
}
static uint32_t obj_type(int i){
    if (i < NAXES) return DIDFT_ABSAXIS | ((uint32_t)i << 8);
    if (i < NAXES + NPOVS) return DIDFT_POV | ((uint32_t)(i - NAXES) << 8);
    return DIDFT_PSHBUTTON | ((uint32_t)(i - NAXES - NPOVS) << 8);
}
/* The names a controls screen shows; positional buttons, so a PlayStation
 * pad's A is "Cross" and a Nintendo pad's A is its bottom button "B".  A
 * wheel's are the ones the page puts there (web/src/input/wheel.ts): its
 * face buttons, the paddles on LB and RB, the pedals on the triggers. */
static const char *wheel_obj_name(int i, int fam){
    static const char *const ax[NAXES] = {"Wheel", "Y Axis", "Brake Pedal", "X Rotation", "Y Rotation", "Gas Pedal"};
    static const char *const btn[3][NBTNS] = {
        {"A", "B", "X", "Y", "Left Paddle", "Right Paddle", "Back", "Start", "L3", "R3",
         "Brake Pedal Full", "Gas Pedal Full"},
        {"A", "B", "X", "Y", "Left Paddle", "Right Paddle", "View", "Menu", "LSB", "RSB",
         "Brake Pedal Full", "Gas Pedal Full"},
        {"Cross", "Circle", "Square", "Triangle", "Left Paddle", "Right Paddle", "Share", "Options", "L3", "R3",
         "Brake Pedal Full", "Gas Pedal Full"} };
    if (i < NAXES) return ax[i];
    if (i == NAXES) return "D-pad";
    if (i < NAXES + NPOVS) return "Wheel Directions";
    int j = btn_name_slot(i - NAXES - NPOVS);
    return j < 0 ? "Button" : btn[fam == PADF_XBOX ? 1 : fam == PADF_PLAYSTATION ? 2 : 0][j];
}
static const char *obj_name(int i, int fam, int wheel){
    if (wheel) return wheel_obj_name(i, fam);
    static const char *const ax[NAXES] = {"Left Stick X", "Left Stick Y", "Left Trigger",
                                          "Right Stick X", "Right Stick Y", "Right Trigger"};
    static const char *const trig[4][2] = {{"Left Trigger", "Right Trigger"}, {"LT", "RT"},
                                           {"L2", "R2"}, {"ZL", "ZR"}};
    static const char *const btn[4][NBTNS] = {
        {"A", "B", "X", "Y", "LB", "RB", "Back", "Start", "Left Stick", "Right Stick",
         "Left Trigger Full", "Right Trigger Full"},
        {"A", "B", "X", "Y", "LB", "RB", "View", "Menu", "Left Stick", "Right Stick",
         "LT Full", "RT Full"},
        {"Cross", "Circle", "Square", "Triangle", "L1", "R1", "Create", "Options", "L3", "R3",
         "L2 Full", "R2 Full"},
        {"B", "A", "Y", "X", "L", "R", "Minus", "Plus", "Left Stick", "Right Stick",
         "ZL Full", "ZR Full"} };
    if (fam < 0 || fam > 3) fam = 0;
    if (i == 2) return trig[fam][0];
    if (i == 5) return trig[fam][1];
    if (i < NAXES) return ax[i];
    if (i == NAXES) return "D-pad";
    if (i < NAXES + NPOVS) return "Left Stick Directions";
    int j = btn_name_slot(i - NAXES - NPOVS);
    return j < 0 ? "Button" : btn[fam][j];
}
static int obj_by_ofs(uint32_t ofs){
    for (int i = 0; i < NOBJS; i++) if (obj_ofs(i) == ofs) return i;
    return -1;
}
static int obj_by_id(uint32_t id){
    for (int i = 0; i < NOBJS; i++){
        uint32_t t = obj_type(i);
        if (((t >> 8) & 0xFFFFu) == ((id >> 8) & 0xFFFFu) && (t & id & DIDFT_TYPEMASK)) return i;
    }
    return -1;
}
static int obj_find(uint32_t obj, uint32_t how){
    return how == 1u ? obj_by_ofs(obj) : how == 2u ? obj_by_id(obj) : -1;
}

/* ---- GUIDs and the device instance ------------------------------------------ */
static void put_guid(uint32_t p, uint32_t d1, uint16_t d2, uint16_t d3, const uint8_t d4[8]){
    MR32(p) = d1; MR16(p + 4) = d2; MR16(p + 6) = d3;
    for (int i = 0; i < 8; i++) MR8(p + 8 + i) = d4[i];
}
/* guidInstance never changes: the game keys saved bindings on it.  The
 * wheel's differs from the pad's in its last byte (2), so each keeps its own. */
static const uint8_t k_inst_d4[8] = {0x80, 0x00, 'N', 'F', 'S', 'U', '2', 0x01};
#define INST_D1 0x50414447u               /* "GDAP" */
static void put_instance_guid(uint32_t p, int wheel){
    put_guid(p, INST_D1, 0x4E46, 0x11F0, k_inst_d4);
    if (wheel) MR8(p + 15) = 0x02;
}
/* 1: the pad's guidInstance, 2: the wheel's, 0: neither */
int dipad_guid(uint32_t g){
    if (!g || !mem_is_committed(g + 15) || MR32(g) != INST_D1) return 0;
    if (memcmp(g_mem + g + 8, k_inst_d4, 7) || MR16(g + 4) != 0x4E46 || MR16(g + 6) != 0x11F0) return 0;
    return MR8(g + 15) == 0x01 ? 1 : MR8(g + 15) == 0x02 ? 2 : 0;
}
/* A HID-style product GUID {PIDVID-0000-0000-0000-504944564944}; the VID/PID
 * collide with none of the game's own lists (0x7fe7f8, 0x800588, 0x5cef00). */
static uint32_t family_vidpid(int fam, int wheel){
    if (wheel) return 0x55451209u;                    /* pid.codes test range, as the generic pad */
    switch (fam){
    case PADF_XBOX:        return 0x0B13045Eu;        /* Xbox Wireless Controller */
    case PADF_PLAYSTATION: return 0x0CE6054Cu;        /* DualSense */
    case PADF_NINTENDO:    return 0x2009057Eu;        /* Switch Pro Controller */
    default:               return 0x55441209u;        /* pid.codes test range */
    }
}
/* the device instance: the pad, or the wheel (wheel), with the name of the controller driving it */
static void write_devinst(uint32_t p, uint32_t size, const PadState *s, int wheel){
    static const uint8_t pidvid[8] = {0x00, 0x00, 'P', 'I', 'D', 'V', 'I', 'D'};
    if (size > 0x244u || size < 0x24u) size = 0x244u;
    mem_commit(p, size);
    memset(g_mem + p, 0, size);
    MR32(p) = size;
    put_instance_guid(p + 0x04, wheel);
    put_guid(p + 0x14, family_vidpid(s->family, wheel), 0, 0, pidvid);
    if (size < 0x28u) return;
    MR32(p + 0x24) = wheel ? WHEEL_DEVTYPE : PAD_DEVTYPE;
    const char *nm = s->connected && s->name[0] && !!s->wheel == !!wheel ? s->name : wheel ? "Steering Wheel" : "Gamepad";
    if (size >= 0x12cu) snprintf((char *)(g_mem + p + 0x28), 260, "%s", nm);
    if (size >= 0x230u) snprintf((char *)(g_mem + p + 0x12c), 260, "%s", nm);
    if (size >= 0x244u){ MR16(p + 0x240) = 1; MR16(p + 0x242) = wheel ? 4 : 5; }   /* Generic Desktop / Joystick, Game Pad */
    int dipad_ff_on(void);
    if (size >= 0x240u && dipad_ff_on()) put_guid(p + 0x230, 0x4646504Eu, 0x4E46, 0x11F0, k_inst_d4);   /* guidFFDriver */
}

/* ---- per-device state ---------------------------------------------------------- */
typedef struct {
    uint32_t obj;                        /* guest IDirectInputDevice8 */
    int32_t  mn[NAXES], mx[NAXES];       /* DIPROP_RANGE */
    uint32_t dz[NAXES], sat[NAXES];      /* DIPROP_DEADZONE / _SATURATION, 0..10000 */
    uint32_t bufsize, axismode, autocenter;
    int      pov1;                       /* left-stick direction with hysteresis */
    uint32_t last_btn, fe_prev;
    int      fe_down[3];                 /* Enter / Escape / Q keystroke is down */
    int      rest;                       /* hotplug: polls answered at rest since the unplug */
    int      wheel;                      /* created as the wheel (its guidInstance), not the pad */
    int      gone;                       /* answered DIERR_INPUTLOST: the game let it go */
    int      trig[2];                    /* buttons 10/11 (LT, RT as buttons) are down */
} Dev;
static Dev g_dev[4];
static Dev *dev_of(uint32_t obj, int create){
    for (int i = 0; i < 4; i++) if (g_dev[i].obj == obj) return &g_dev[i];
    if (!create) return 0;
    /* a free slot, else the oldest: in hotplug mode every replug is a new
     * device object and the released ones are never called again */
    static int next = 0;
    Dev *d = 0;
    for (int i = 0; i < 4 && !d; i++) if (!g_dev[i].obj) d = &g_dev[i];
    if (!d) d = &g_dev[next++ & 3];
    memset(d, 0, sizeof *d); d->obj = obj;
    for (int a = 0; a < NAXES; a++){ d->mn[a] = 0; d->mx[a] = 65535; d->sat[a] = 10000; }
    return d;
}
/* CreateDevice with one of our guidInstances (guid: the pad's or the wheel's) */
void dipad_created(uint32_t obj, uint32_t guid){
    Dev *d = dev_of(obj, 1);
    d->wheel = dipad_guid(guid) == 2;
    PadState s; gamepad_read(&s);
    if (pad_log()) fprintf(stderr, "[pad] CreateDevice -> 0x%08x, the %s (%s)\n", obj, d->wheel ? "wheel" : "pad",
                           s.connected ? s.name : "no controller yet");
}

/* ---- enumeration ------------------------------------------------------------- */
static int g_kind = -1;                  /* the device last offered: 0 the pad, 1 the wheel (-1: none yet) */
static int g_seen;                       /* a device was offered */
static struct { uint32_t cb, ref, type; } g_late;   /* the enumeration that found none (game.h pad_late) */
/* the device instance an enumeration offers: the pad, or the wheel (wheel) */
static uint32_t offer(const PadState *s, int wheel){
    static uint32_t ddi = 0;
    if (!ddi) ddi = com_scratch(0x260);
    g_seen = 1;
    g_kind = wheel;
    write_devinst(ddi, 0x244u, s, wheel);
    return ddi;
}
/* IDirectInput8::EnumDevices: offer the pad for DI8DEVCLASS_ALL, _GAMECTRL
 * and DI8DEVTYPE_GAMEPAD, or the wheel (a wheel drives the pad) for _ALL,
 * _GAMECTRL and DI8DEVTYPE_DRIVING.  Returns 1 when the callback said
 * DIENUM_STOP. */
int dipad_enum(CPU *c, uint32_t devtype, uint32_t cb, uint32_t ref, uint32_t flags){
    int mode = pad_mode();
    if (mode == MODE_OFF || !cb) return 0;
    int dipad_ff_on(void);
    if ((flags & 0x100u) && !dipad_ff_on()) return 0;        /* DIEDFL_FORCEFEEDBACK */
    PadState s; gamepad_read(&s);
    /* a device of the other kind is being let go (dipad_call): the new one
     * comes with the rescan after it is gone */
    for (int i = 0; i < 4; i++) if (g_dev[i].obj && !g_dev[i].gone && g_dev[i].rest > 0) return 0;
    int wheel = s.connected ? !!s.wheel : g_kind > 0;
    uint32_t t = devtype & 0xFFu;
    if (t != 0u && t != 4u && t != (wheel ? DI8DEVTYPE_DRIVING : DI8DEVTYPE_GAMEPAD)) return 0;
    /* The game's own search (game.h pad_rescan) finds a controller plugged in
     * later.  Hotplug mode keeps it on.  The default mode offers the device
     * once a controller has been seen and keeps it from then on (the search
     * off again): with no controller there is no game pad, as in the original
     * game.  A device that nothing drives would leave a keyboard player stuck
     * on UG2's Joypad page "Press a button" prompt, which only a pad button
     * ends. */
    if (mode == MODE_HOTPLUG || !g_seen){
        pad_rescan(mode == MODE_HOTPLUG || !s.connected);
        if (!s.connected){
            /* a game with no search of its own takes a controller connected
             * later through the callback it names (dipad_present) */
            if (g_game.pad_late && cb == g_game.pad_late->cb && !g_seen){ g_late.cb = cb; g_late.ref = ref; g_late.type = t; }
            return 0;
        }
    }
    else pad_rescan(0);                                     /* the search after a change of kind: done */
    uint32_t ddi = offer(&s, wheel);
    static int nlog = 0;
    if (pad_log() && nlog < 8){ nlog++;
        fprintf(stderr, "[pad] EnumDevices(type 0x%x) -> \"%s\"\n", devtype, (char *)(g_mem + ddi + 0x12c)); }
    uint32_t args[2] = { ddi, ref };
    return call_guest(c, cb, 2, args) == 0u;
}

/* Every Present (com.c), on the game's thread: a controller connected since
 * the game's enumeration found none, for a game that takes it through that
 * enumeration's callback (game.h pad_late): the callback gets the
 * device as the enumeration would have offered it, with its pvRef, once.  A
 * Present is outside the game's input update, so the device the callback
 * makes joins the game's list between two of its polls. */
void dipad_present(CPU *c){
    if (!g_late.cb || g_seen) return;
    PadState s; gamepad_read(&s);
    if (!s.connected) return;
    int wheel = !!s.wheel;
    uint32_t t = g_late.type, cb = g_late.cb, ref = g_late.ref;
    if (t != 0u && t != 4u && t != (wheel ? DI8DEVTYPE_DRIVING : DI8DEVTYPE_GAMEPAD)) return;
    g_late.cb = 0;
    uint32_t ddi = offer(&s, wheel);
    fprintf(stderr, "[pad] %s connected after the game's enumeration: \"%s\" offered to its callback 0x%08x\n",
            s.name, (char *)(g_mem + ddi + 0x12c), cb);
    uint32_t args[2] = { ddi, ref };
    call_guest(c, cb, 2, args);
    /* the game's own set-up for the controllers it found, again: it ran at
     * the start with none */
    const PadLate *pl = g_game.pad_late;
    if (!pl->setup) return;
    uint32_t obj = mem_is_committed(pl->setup_this + 3u) ? MR32(pl->setup_this) : 0u;
    if (!obj || !mem_is_committed(obj + 3u) || MR32(obj) != pl->setup_vtbl){
        fprintf(stderr, "[pad] the game's input set-up 0x%08x not run again: no object of vtable 0x%08x at [0x%08x]\n",
                pl->setup, pl->setup_vtbl, pl->setup_this);
        return;
    }
    uint32_t ecx = c->ecx;
    c->ecx = obj;
    call_guest(c, pl->setup, 0, 0);
    c->ecx = ecx;
    if (pad_log()) fprintf(stderr, "[pad] the game's input set-up 0x%08x run again on 0x%08x\n", pl->setup, obj);
}

/* ---- state ---------------------------------------------------------------------- */
/* t in -1..1 to the device range, with DirectInput's dead zone and
 * saturation (fractions of the distance from the centre, 0..10000). */
static int32_t scale_axis(float t, const Dev *d, int a){
    if (t > 1.f) t = 1.f; if (t < -1.f) t = -1.f;
    double dz = d->dz[a] > 10000u ? 10000.0 : d->dz[a];
    double sat = d->sat[a] > 10000u ? 10000.0 : d->sat[a];
    if (sat < dz) sat = dz;
    double mag = (t < 0 ? -t : t) * 10000.0, f;
    if (mag <= dz) f = 0; else if (mag >= sat) f = 1; else f = (mag - dz) / (sat - dz);
    int64_t lo = d->mn[a], hi = d->mx[a], mid = lo + (hi - lo + 1) / 2;
    int64_t v = t >= 0 ? mid + (int64_t)(f * (double)(hi - mid) + 0.5)
                       : mid - (int64_t)(f * (double)(mid - lo) + 0.5);
    return (int32_t)v;
}
static uint32_t pov_of(int up, int down, int left, int right){
    int x = right - left, y = down - up;
    static const uint32_t T[3][3] = {{31500u, 0u, 4500u}, {27000u, 0xFFFFFFFFu, 9000u},
                                     {22500u, 18000u, 13500u}};
    return T[y + 1][x + 1];
}
/* The left stick as a four-way hat for the menus: the dominant direction
 * past 0.55, released below 0.40. */
static uint32_t stick_pov(Dev *d, float x, float y){
    float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    int dir = d->pov1;                                     /* 0 none, 1 up, 2 right, 3 down, 4 left */
    if (dir && (ax > ay ? ax : ay) < 0.40f) dir = 0;
    if (!dir && (ax > 0.55f || ay > 0.55f))
        dir = ax > ay ? (x > 0 ? 2 : 4) : (y > 0 ? 3 : 1);
    if (dir == 2 || dir == 4){ if (ay > ax + 0.2f) dir = y > 0 ? 3 : 1; }
    else if (dir){ if (ax > ay + 0.2f) dir = x > 0 ? 2 : 4; }
    d->pov1 = dir;
    static const uint32_t P[5] = {0xFFFFFFFFu, 0u, 9000u, 18000u, 27000u};
    return P[dir];
}
static void fill_state(uint8_t *buf, uint32_t size, Dev *d, const PadState *s){
    uint8_t st[0x110];
    memset(st, 0, sizeof st);
    for (int a = 0; a < NAXES; a++){
        int src = k_axis_src[a];
        float t = s->axis[src];
        if (src >= PAD_AXIS_LT) t = t * 2.f - 1.f;          /* triggers rest at the minimum */
        int32_t v = scale_axis(t, d, a);
        memcpy(st + k_axis_ofs[a], &v, 4);
    }
    uint32_t b = s->buttons;
    uint32_t pov[4] = { pov_of(!!(b & 1u << PAD_DUP), !!(b & 1u << PAD_DDOWN),
                               !!(b & 1u << PAD_DLEFT), !!(b & 1u << PAD_DRIGHT)),
                        stick_pov(d, s->axis[PAD_AXIS_LX], s->axis[PAD_AXIS_LY]),
                        0xFFFFFFFFu, 0xFFFFFFFFu };
    memcpy(st + 0x20, pov, sizeof pov);
    static float click = -1.f;
    if (click < 0.f){ const char *e = getenv("NFSU2_PAD_TRIGGER_CLICK");
                      click = e && *e ? (float)atof(e) : 0.97f;
                      if (click < 0.05f || click > 1.f) click = 0.97f; }
    /* A trigger the game reads as a pedal presses its button at the end of
     * its travel (the digital top of the pedal, see the default layout).  One
     * it reads as no axis is a plain button (shifting, in UG2's PS2 layout):
     * it presses at half travel and lets go below 0.4.  Which is which is the
     * game's to say (game.h pad_trigger_axes); a game that does not say has
     * both read as axes. */
    unsigned axes = g_game.pad_trigger_axes ? g_game.pad_trigger_axes(d->obj) : 3u;
    for (int k = 0; k < 2; k++){
        float v = s->axis[k ? PAD_AXIS_RT : PAD_AXIS_LT];
        if (axes & (1u << k)) d->trig[k] = v >= click;
        else d->trig[k] = v >= (d->trig[k] ? 0.4f : 0.5f);
    }
    for (int i = 0; i < NBTNS; i++){
        int src = btn_src()[i];
        int on = src == SRC_LT_FULL ? d->trig[0] : src == SRC_RT_FULL ? d->trig[1] : !!(b & (1u << src));
        if (on) st[0x30 + i] = 0x80;
    }
    memcpy(buf, st, size < sizeof st ? size : sizeof st);
}

/* ---- front-end keystrokes ----------------------------------------------------------
 * Some of a game's front-end input can exist only as characters from the
 * window procedure, not as DirectInput actions (UG2's text fields and its quit
 * prompt, runtime/pad_ug2.c).  A keyboard key produces both the action and the
 * character.  So for a game that asks (game.h pad_fe_keys), while it is in the
 * front end (its flow state flow_fe) A, B and View also post Enter, Escape
 * and Q.  (In a text field View therefore types a 'q', as the Q key would.)
 * NFSU2_PAD_FE_KEYS=0 (or off) turns this off. */
void win32_post_message(uint32_t msg, uint32_t wp, uint32_t lp);
static void fe_key(int *down, int pressed, int released, int fe, uint32_t vk, uint32_t scan,
                   uint32_t ch){
    uint32_t lp = 1u | (scan << 16);
    if (pressed && fe && !*down){
        win32_post_message(0x0100u, vk, lp);                  /* WM_KEYDOWN */
        win32_post_message(0x0102u, ch, lp);                  /* WM_CHAR */
        *down = 1;
        if (pad_log()) fprintf(stderr, "[pad] front end: keystroke 0x%02x\n", vk);
    } else if (released && *down){
        win32_post_message(0x0101u, vk, lp | 0xC0000000u);    /* WM_KEYUP */
        *down = 0;
    }
}
static void fe_keys(Dev *d, uint32_t buttons){
    static int on = -1;
    if (on < 0) on = g_game.pad_fe_keys && gamepad_env_flag("NFSU2_PAD_FE_KEYS", 1);
    uint32_t press = buttons & ~d->fe_prev, rel = d->fe_prev & ~buttons;
    d->fe_prev = buttons;
    if (!on) return;
    int fe = game_flow() == g_game.flow_fe;
    fe_key(&d->fe_down[0], !!(press & 1u << PAD_A), !!(rel & 1u << PAD_A), fe, 0x0D, 0x1C, 0x0D);
    fe_key(&d->fe_down[1], !!(press & 1u << PAD_B), !!(rel & 1u << PAD_B), fe, 0x1B, 0x01, 0x1B);
    fe_key(&d->fe_down[2], !!(press & 1u << PAD_BACK), !!(rel & 1u << PAD_BACK), fe, 'Q', 0x10, 'q');
}

/* ---- IDirectInputDevice8 methods for the pad --------------------------------------- */
static void write_objinst(uint32_t p, uint32_t size, int i, int fam, int wheel){
    if (!size || size > 0x13cu) size = 0x13cu;
    mem_commit(p, size);
    memset(g_mem + p, 0, size);
    MR32(p) = size;
    static const uint8_t tail[8] = {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
    uint32_t g = i < NAXES ? k_axis_guid[i] : i < NAXES + NPOVS ? 0xA36D02F2u : 0xA36D02F0u;
    put_guid(p + 0x04, g, 0xC9F3, 0x11CF, tail);
    MR32(p + 0x14) = obj_ofs(i);
    int dipad_ff_on(void);
    int act = i < 2 && dipad_ff_on();                              /* X and Y: force-feedback actuators */
    MR32(p + 0x18) = obj_type(i) | (act ? DIDFT_FFACTUATOR : 0u);
    MR32(p + 0x1c) = (i < NAXES ? DIDOI_ASPECTPOSITION : 0u) | (act ? DIDOI_FFACTUATOR : 0u);
    if (act && size >= 0x12cu){ MR32(p + 0x124) = 10000u; MR32(p + 0x128) = 1u; }   /* dwFFMaxForce, dwFFForceResolution */
    if (size >= 0x124u) snprintf((char *)(g_mem + p + 0x20), 260, "%s", obj_name(i, fam, wheel));
    if (size >= 0x134u){
        MR16(p + 0x130) = i >= NAXES + NPOVS ? 9u : 1u;      /* Button / Generic Desktop */
        MR16(p + 0x132) = i < NAXES ? k_axis_usage[i] : i < NAXES + NPOVS ? 0x39u
                        : (uint16_t)(i - NAXES - NPOVS + 1);
    }
}

/* A controls page that lists the devices it found when it opened must not
 * lose one (UG2's crashes as soon as one of the pad's bindings is edited,
 * pad_ug2.c): on the game's screen of that kind (game.h pad_hold_screen) an
 * unplugged pad reads at rest and is lost once the page is left. */
const char *autotest_screen(void);              /* autotest.c */
static int on_controls_page(void){
    const char *sc = g_game.pad_hold_screen ? autotest_screen() : 0;
    return sc && !strcasecmp(sc, g_game.pad_hold_screen);
}

/* ---- force feedback ---------------------------------------------------------------
 * The platform's motors: input_web.c in the browser.  The Mac's gamepad.m
 * drives none, so these weak defaults keep its pad without force feedback. */
__attribute__((weak)) int gamepad_rumble_ok(void){ return 0; }
__attribute__((weak)) void gamepad_rumble(float strong, float weak){ (void)strong; (void)weak; }
uint32_t com_new_dieffect(void);                        /* com.c */

#define FF_INFINITE 0xFFFFFFFFu
enum { EF_CONST = 0x20, EF_RAMP, EF_SQUARE, EF_SINE, EF_TRIANGLE, EF_SAWUP, EF_SAWDOWN,
       EF_SPRING, EF_DAMPER, EF_INERTIA, EF_FRICTION, EF_CUSTOM };   /* GUID_* Data1 low byte */
static const uint8_t k_ff_d4[8] = {0x9A, 0xD0, 0x00, 0xA0, 0xC9, 0xA0, 0x6E, 0x35};
typedef struct {
    uint32_t obj;                       /* the guest IDirectInputEffect; 0 = a free slot */
    int      kind;                      /* EF_* */
    uint32_t dur_us, gain, delay_us;    /* DIEFFECT */
    int32_t  mag, offset;               /* DIPERIODIC dwMagnitude/lOffset, DICONSTANTFORCE, DIRAMPFORCE */
    uint32_t period_us;
    int      env; uint32_t att_lvl, att_us, fade_lvl, fade_us;   /* DIENVELOPE */
    int      playing; uint32_t iters; double t0;
    int      unloaded;                  /* Unload gave its place on the device back (see CreateEffect) */
} Eff;
#define NEFF 64
static Eff g_eff[NEFF];
static uint32_t g_ff_gain = 10000u;     /* DIPROP_FFGAIN */
static int g_ff_paused, g_ff_actoff;    /* SendForceFeedbackCommand */
static int ff_log(void){ static int v = -1; if (v < 0){ const char *e = getenv("NFSU2_FF_LOG"); v = e && *e && *e != '0'; } return v; }
int dipad_ff_log(void){ return ff_log(); }
/* force feedback is reported when the platform can rumble the controller
 * (NFSU2_FF=0: never); decided once, when the game first asks */
int dipad_ff_on(void){
    static int v = -1;
    if (v < 0){
        if (!gamepad_env_flag("NFSU2_FF", 1)) v = 0;
        else if (gamepad_rumble_ok()) v = 1;
        else return 0;                  /* no motors yet: ask again next time */
        if (pad_log() || ff_log()) fprintf(stderr, "[pad] force feedback %s\n", v ? "on: effects play as rumble" : "off (NFSU2_FF=0)");
    }
    return v;
}
static double ff_now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }
static const char *ff_name(int k){
    static const char *const n[] = { "constant", "ramp", "square", "sine", "triangle", "sawtooth up", "sawtooth down",
                                     "spring", "damper", "inertia", "friction", "custom" };
    return k >= EF_CONST && k <= EF_CUSTOM ? n[k - EF_CONST] : "?";
}
static int ff_periodic(int k){ return k >= EF_SQUARE && k <= EF_SAWDOWN; }
static Eff *eff_of(uint32_t obj){
    if (!obj || !mem_is_committed(obj + 7u)) return 0;
    uint32_t i = MR32(obj + 4u);
    return i >= 1u && i <= NEFF && g_eff[i - 1].obj == obj ? &g_eff[i - 1] : 0;
}
static void put_ff_guid(uint32_t p, int k){ put_guid(p, 0x13541C00u | (uint32_t)k, 0x8E33, 0x11D0, k_ff_d4); }
static int guid_kind(uint32_t g){
    if (!g || !mem_is_committed(g + 15u)) return 0;
    uint32_t d1 = MR32(g);
    if ((d1 & 0xFFFFFF00u) != 0x13541C00u || MR16(g + 4) != 0x8E33u || MR16(g + 6) != 0x11D0u ||
        memcmp(g_mem + g + 8, k_ff_d4, 8)) return 0;
    int k = (int)(d1 & 0xFFu);
    return k >= EF_CONST && k <= EF_CUSTOM ? k : 0;
}
/* DIEFFECT (dwSize 0x38 for DirectX 5, 0x44 since 6): the fields DIEP_* names */
#define DIEP_DURATION 0x1u
#define DIEP_GAIN 0x4u
#define DIEP_ENVELOPE 0x80u
#define DIEP_TYPESPECIFICPARAMS 0x100u
#define DIEP_STARTDELAY 0x200u
#define DIEP_START 0x20000000u
static void eff_start(Eff *e, uint32_t iters, uint32_t flags){
    if (flags & 1u)                                     /* DIES_SOLO */
        for (int i = 0; i < NEFF; i++) if (&g_eff[i] != e) g_eff[i].playing = 0;
    e->playing = 1; e->iters = iters ? iters : 1u; e->t0 = ff_now();
    if (ff_log()) fprintf(stderr, "[ff] start %s #%d x%u (magnitude %d, period %u us, duration %u us, gain %u)\n",
                          ff_name(e->kind), (int)(e - g_eff), e->iters, e->mag, e->period_us, e->dur_us, e->gain);
}
static uint32_t eff_set(Eff *e, uint32_t p, uint32_t fl){
    if (!p || !mem_is_committed(p + 0x33u)) return DIERR_INVALIDPARAM;
    uint32_t sz = MR32(p);
    if (fl & DIEP_DURATION) e->dur_us = MR32(p + 0x08);
    if (fl & DIEP_GAIN){ uint32_t g = MR32(p + 0x10); e->gain = g > 10000u ? 10000u : g; }
    if (fl & DIEP_ENVELOPE){
        uint32_t ev = MR32(p + 0x28);
        e->env = ev && mem_is_committed(ev + 19u);
        if (e->env){ e->att_lvl = MR32(ev + 4); e->att_us = MR32(ev + 8); e->fade_lvl = MR32(ev + 12); e->fade_us = MR32(ev + 16); }
    }
    if (fl & DIEP_TYPESPECIFICPARAMS){
        uint32_t cb = MR32(p + 0x2c), tp = MR32(p + 0x30);
        if (tp && cb && mem_is_committed(tp + cb - 1u)){
            if (ff_periodic(e->kind) && cb >= 16u){
                e->mag = (int32_t)MR32(tp); e->offset = (int32_t)MR32(tp + 4); e->period_us = MR32(tp + 12);
            } else if (e->kind == EF_CONST && cb >= 4u) e->mag = (int32_t)MR32(tp);
            else if (e->kind == EF_RAMP && cb >= 8u){
                int32_t a = (int32_t)MR32(tp), b = (int32_t)MR32(tp + 4);
                e->mag = abs(a) > abs(b) ? a : b;
            }
        }
    }
    if ((fl & DIEP_STARTDELAY) && sz >= 0x38u) e->delay_us = MR32(p + 0x34);
    if (ff_log()) fprintf(stderr, "[ff] parameters %s #%d: flags 0x%x magnitude %d offset %d period %u us duration %u us gain %u delay %u us"
                          " envelope %d (attack %u over %u us, fade %u over %u us)\n",
                          ff_name(e->kind), (int)(e - g_eff), fl, e->mag, e->offset, e->period_us, e->dur_us, e->gain, e->delay_us,
                          e->env, e->att_lvl, e->att_us, e->fade_lvl, e->fade_us);
    if (fl & DIEP_START) eff_start(e, 1u, 0u);
    return DI_OK;
}
/* Every frame (GetDeviceState): the running effects as two motor levels.
 * A periodic effect is a vibration: slow ones (period 60 ms and more, 16 Hz
 * and less) on the strong, low-frequency motor, fast ones (25 ms and less)
 * on the weak one, those between on both.  A constant or ramp force is what
 * pushes a wheel; on a pad it is a light rumble on the weak motor, with its
 * strength.  Springs, dampers, inertia and friction centre or weigh a wheel
 * and are not played.  Effect gain x device gain x the envelope. */
static float g_ff_last[2] = { -1.f, -1.f };
static void ff_mix(void){
    if (!dipad_ff_on()) return;
    double now = ff_now();
    double strong = 0, weak = 0;
    for (int i = 0; i < NEFF; i++){
        Eff *e = &g_eff[i];
        if (!e->obj || !e->playing || g_ff_paused) continue;
        double t = (now - e->t0) * 1e6 - (double)e->delay_us;
        if (t < 0) continue;
        double dur = e->dur_us == FF_INFINITE ? INFINITY : (double)e->dur_us;
        if (isfinite(dur)){
            double total = e->iters == FF_INFINITE ? INFINITY : dur * (double)e->iters;
            if (t >= total){ e->playing = 0; continue; }
            if (dur > 0) t = fmod(t, dur);
        }
        double lvl = ff_periodic(e->kind) ? fabs((double)e->mag) + fabs((double)e->offset) : fabs((double)e->mag);
        if (e->env){
            if (e->att_us && t < (double)e->att_us) lvl = (double)e->att_lvl + (lvl - (double)e->att_lvl) * t / (double)e->att_us;
            else if (e->fade_us && isfinite(dur) && t > dur - (double)e->fade_us)
                lvl = (double)e->fade_lvl + (lvl - (double)e->fade_lvl) * (dur - t) / (double)e->fade_us;
        }
        double a = lvl / 10000.0 * (double)e->gain / 10000.0 * (double)g_ff_gain / 10000.0;
        if (a <= 0) continue;
        if (ff_periodic(e->kind)){
            if (e->period_us >= 60000u) strong += a;
            else if (e->period_us <= 25000u) weak += a;
            else { strong += a * 0.5; weak += a * 0.5; }
        } else if (e->kind == EF_CONST || e->kind == EF_RAMP) weak += a * 0.5;
    }
    float jolt = g_game.pad_jolt ? g_game.pad_jolt() : 0.f;    /* the game's own jolts (UG2: the player car's) */
    strong += jolt; weak += jolt * 0.4f;
    if (g_ff_actoff){ strong = 0; weak = 0; }
    float s = strong > 1 ? 1.f : (float)strong, w = weak > 1 ? 1.f : (float)weak;
    gamepad_rumble(s, w);
    if (ff_log() && (fabsf(s - g_ff_last[0]) > 0.05f || fabsf(w - g_ff_last[1]) > 0.05f || ((s == 0.f) != (g_ff_last[0] == 0.f)) || ((w == 0.f) != (g_ff_last[1] == 0.f)))){
        fprintf(stderr, "[ff] motors strong %.2f weak %.2f\n", (double)s, (double)w);
        g_ff_last[0] = s; g_ff_last[1] = w;
    }
}
static void effinfo(uint32_t p, uint32_t size, int k){
    if (size < 0x20u || size > 0x124u) size = 0x124u;
    mem_commit(p, size); memset(g_mem + p, 0, size);
    MR32(p) = size; put_ff_guid(p + 4, k);
    uint32_t type = k == EF_CONST ? 1u : k == EF_RAMP ? 2u : ff_periodic(k) ? 3u : k == EF_CUSTOM ? 5u : 4u;
    if (type <= 3u) type |= 0x200u | 0x400u | 0x8000u;      /* DIEFT_FFATTACK | FFFADE | STARTDELAY */
    MR32(p + 0x14) = type;
    MR32(p + 0x18) = MR32(p + 0x1c) = 0x3FFu;              /* DIEP_ALLPARAMS */
    if (size >= 0x124u) snprintf((char *)(g_mem + p + 0x20), 260, "%s", ff_name(k));
}
/* the device's force-feedback methods; -1 = not one of them */
static int64_t ff_dev_call(CPU *c, const char *nm, const uint32_t *a){
    if (!strcmp(nm, "CreateEffect")){
        uint32_t g = a[1], peff = a[2], pp = a[3];
        if (pp && mem_is_committed(pp + 3)) MR32(pp) = 0;
        if (!dipad_ff_on()) return DIERR_UNSUPPORTED;
        int k = guid_kind(g);
        if (!k || !pp) return DIERR_INVALIDPARAM;
        /* The game changes a rumble by Unload (0x5bfff0) and a new effect, and
         * never releases the old one: on Windows Unload frees the effect's
         * place in the device, so an unloaded effect's slot is taken once
         * none is free (64 changes in, every CreateEffect failed, and the game
         * logged two errors every frame from then on). */
        int slot = -1;
        for (int i = 0; i < NEFF; i++) if (!g_eff[i].obj){ slot = i; break; }
        for (int i = 0; slot < 0 && i < NEFF; i++) if (g_eff[i].unloaded) slot = i;
        if (slot < 0){
            static int said;
            if (!said++) fprintf(stderr, "[ff] CreateEffect: all %d effects are in use (DIERR_DEVICEFULL)\n", NEFF);
            return 0x80040208u;                         /* DIERR_DEVICEFULL */
        }
        uint32_t obj = com_new_dieffect();
        if (!obj) return 0x8007000Eu;                   /* E_OUTOFMEMORY */
        Eff *e = &g_eff[slot];
        int reused = e->obj != 0;               /* the old object stays, with no slot (eff_of: 0) */
        memset(e, 0, sizeof *e);
        e->obj = obj; e->kind = k; e->gain = 10000u; e->dur_us = FF_INFINITE;
        MR32(obj + 4u) = (uint32_t)slot + 1u;
        if (ff_log()) fprintf(stderr, "[ff] CreateEffect %s -> #%d 0x%08x%s\n", ff_name(k), slot, obj, reused ? " (an unloaded effect's place)" : "");
        if (peff) eff_set(e, peff, 0x3FFu);
        MR32(pp) = obj;
        return DI_OK;
    }
    if (!strcmp(nm, "EnumEffects")){
        uint32_t cb = a[1], ref = a[2], want = a[3] & 0xFFu;
        if (!cb || !dipad_ff_on()) return DI_OK;
        static uint32_t ei = 0;
        if (!ei) ei = com_scratch(0x124);
        for (int k = EF_CONST; k <= EF_DAMPER; k++){
            uint32_t type = k == EF_CONST ? 1u : k == EF_RAMP ? 2u : ff_periodic(k) ? 3u : 4u;
            if (want && want != type) continue;
            effinfo(ei, 0x124u, k);
            uint32_t args[2] = { ei, ref };
            if (call_guest(c, cb, 2, args) == 0u) break;    /* DIENUM_STOP */
        }
        return DI_OK;
    }
    if (!strcmp(nm, "GetEffectInfo")){
        uint32_t p = a[1]; int k = guid_kind(a[2]);
        if (!dipad_ff_on()) return DIERR_UNSUPPORTED;
        if (!k || !p || !mem_is_committed(p + 3)) return DIERR_INVALIDPARAM;
        effinfo(p, MR32(p), k);
        return DI_OK;
    }
    if (!strcmp(nm, "GetForceFeedbackState")){
        uint32_t p = a[1];
        if (!dipad_ff_on()) return DIERR_UNSUPPORTED;
        if (!p || !mem_is_committed(p + 3)) return DIERR_INVALIDPARAM;
        int any = 0;
        for (int i = 0; i < NEFF; i++) if (g_eff[i].obj && g_eff[i].playing) any = 1;
        MR32(p) = (any ? 0u : 2u) | (g_ff_paused ? 4u : 0u) | (g_ff_actoff ? 0x20u : 0x10u) | 0x40u | 0x400u;
        return DI_OK;                                   /* STOPPED, PAUSED, ACTUATORSON/OFF, POWERON, USERFFSWITCHON */
    }
    if (!strcmp(nm, "SendForceFeedbackCommand")){
        uint32_t f = a[1];
        if (!dipad_ff_on()) return DI_OK;
        if (f & 1u) for (int i = 0; i < NEFF; i++) g_eff[i].playing = 0;      /* RESET */
        if (f & 2u) for (int i = 0; i < NEFF; i++) g_eff[i].playing = 0;      /* STOPALL */
        if (f & 4u) g_ff_paused = 1;
        if (f & 8u) g_ff_paused = 0;
        if (f & 0x10u) g_ff_actoff = 0;
        if (f & 0x20u) g_ff_actoff = 1;
        if (ff_log()) fprintf(stderr, "[ff] SendForceFeedbackCommand 0x%x\n", f);
        if (f & 0x23u) ff_mix();
        return DI_OK;
    }
    if (!strcmp(nm, "EnumCreatedEffectObjects")){
        uint32_t cb = a[1], ref = a[2];
        if (!cb) return DIERR_INVALIDPARAM;
        for (int i = 0; i < NEFF; i++) if (g_eff[i].obj){
            uint32_t args[2] = { g_eff[i].obj, ref };
            if (call_guest(c, cb, 2, args) == 0u) break;
        }
        return DI_OK;
    }
    return -1;
}
/* IDirectInputEffect (com.c dispatches here; IUnknown is com.c's) */
uint32_t dipad_effect_call(CPU *c, uint32_t self, const char *nm, int nargs, const uint32_t *a){
    (void)c; (void)nargs;
    Eff *e = eff_of(self);
    if (!e) return DIERR_INVALIDPARAM;
    /* SetParameters, Start and Download put an unloaded effect back on the device */
    if (!strcmp(nm, "SetParameters")){ e->unloaded = 0; return eff_set(e, a[1], a[2]); }
    if (!strcmp(nm, "Start")){ e->unloaded = 0; eff_start(e, a[1], a[2]); return DI_OK; }
    if (!strcmp(nm, "Stop") || !strcmp(nm, "Unload")){
        if (ff_log() && e->playing) fprintf(stderr, "[ff] stop %s #%d\n", ff_name(e->kind), (int)(e - g_eff));
        e->playing = 0;
        if (nm[0] == 'U') e->unloaded = 1;
        return DI_OK;
    }
    if (!strcmp(nm, "GetEffectStatus")){
        uint32_t p = a[1];
        if (!p || !mem_is_committed(p + 3)) return DIERR_INVALIDPARAM;
        MR32(p) = e->playing ? 1u : 0u;                 /* DIEGES_PLAYING */
        return DI_OK;
    }
    if (!strcmp(nm, "GetEffectGuid")){
        uint32_t p = a[1];
        if (!p || !mem_is_committed(p + 15)) return DIERR_INVALIDPARAM;
        put_ff_guid(p, e->kind); return DI_OK;
    }
    if (!strcmp(nm, "GetParameters")){
        uint32_t p = a[1], fl = a[2];
        if (!p || !mem_is_committed(p + 0x33u)) return DIERR_INVALIDPARAM;
        if (fl & DIEP_DURATION) MR32(p + 0x08) = e->dur_us;
        if (fl & DIEP_GAIN) MR32(p + 0x10) = e->gain;
        if ((fl & DIEP_STARTDELAY) && MR32(p) >= 0x38u) MR32(p + 0x34) = e->delay_us;
        if (fl & DIEP_TYPESPECIFICPARAMS){
            uint32_t cb = MR32(p + 0x2c), tp = MR32(p + 0x30);
            if (tp && ff_periodic(e->kind) && cb >= 16u && mem_is_committed(tp + 15u)){
                MR32(tp) = (uint32_t)e->mag; MR32(tp + 4) = (uint32_t)e->offset; MR32(tp + 8) = 0; MR32(tp + 12) = e->period_us;
            } else if (tp && cb >= 4u && mem_is_committed(tp + 3u)) MR32(tp) = (uint32_t)e->mag;
        }
        return DI_OK;
    }
    if (!strcmp(nm, "Download")){ e->unloaded = 0; return DI_OK; }
    if (!strcmp(nm, "Initialize")) return DI_OK;
    return DIERR_UNSUPPORTED;                           /* Escape */
}
void dipad_effect_free(uint32_t obj){
    Eff *e = eff_of(obj);
    if (!e) return;
    if (ff_log()) fprintf(stderr, "[ff] release %s #%d\n", ff_name(e->kind), (int)(e - g_eff));
    memset(e, 0, sizeof *e);
}

uint32_t dipad_call(CPU *c, uint32_t self, const char *nm, int nargs, const uint32_t *a){
    Dev *d = dev_of(self, 1);
    PadState s; gamepad_read(&s);
    /* a controller of the other kind drives the pad (a wheel, and this is the
     * pad, or back): this device makes way for one of that kind, where the
     * game's search will find that one (game.h pad_rescan) */
    int other = g_game.pad_rescan && s.connected && !!s.wheel != d->wheel;
    int lost = ((pad_mode() == MODE_HOTPLUG && !s.connected) || other) && !on_controls_page();
    (void)nargs;
    if (!strcmp(nm, "GetDeviceState")){
        uint32_t cb = a[1], p = a[2];
        /* Unplugged (hotplug mode), or the other kind: answer two polls with
         * the pad at rest before DIERR_INPUTLOST.  UG2 drops the device's
         * channel with its last values, which then stay applied (steering,
         * throttle, held buttons) until its next 32-frame rescan, about
         * 0.3 s, which then finds the device of the new kind. */
        if (lost && d->rest >= 2){
            if (other && !d->gone){
                pad_rescan(1);
                fprintf(stderr, "[pad] %s drives the pad: the game's %s device makes way for a %s\n",
                        s.name, d->wheel ? "wheel" : "pad", s.wheel ? "wheel" : "pad");
            }
            d->gone = 1;
            return DIERR_INPUTLOST;
        }
        d->rest = lost ? d->rest + 1 : 0;
        if (!p || !cb || cb > 0x110u) return DIERR_INVALIDPARAM;
        mem_commit(p, cb);
        PadState at = s;
        if (lost) memset(&at, 0, sizeof at);
        fill_state(g_mem + p, cb, d, &at);
        ff_mix();
        fe_keys(d, at.buttons);
        if (pad_log() && s.buttons != d->last_btn){
            fprintf(stderr, "[pad] buttons 0x%04x  LX %+.2f LY %+.2f LT %.2f RT %.2f\n", s.buttons,
                    (double)s.axis[PAD_AXIS_LX], (double)s.axis[PAD_AXIS_LY],
                    (double)s.axis[PAD_AXIS_LT], (double)s.axis[PAD_AXIS_RT]);
            d->last_btn = s.buttons;
        }
        return DI_OK;
    }
    /* An unplugged pad fails GetDeviceState only: 0x5c8700 polls first and,
     * when Poll fails, just re-acquires and returns, so only a failing
     * GetDeviceState reaches the game's release path (0x5c883f). */
    if (!strcmp(nm, "Poll"))    return DI_NOEFFECT;
    if (!strcmp(nm, "Acquire")) return DI_OK;
    if (!strcmp(nm, "Unacquire") || !strcmp(nm, "SetCooperativeLevel") ||
        !strcmp(nm, "SetEventNotification")) return DI_OK;
    if (!strcmp(nm, "SetDataFormat")){
        uint32_t df = a[1];
        uint32_t sz = (df && mem_is_committed(df + 15)) ? MR32(df + 12) : 0;
        if (sz != 0x110u && sz != 0x50u){
            fprintf(stderr, "[pad] SetDataFormat: unsupported data size %u\n", sz);
            return DIERR_INVALIDPARAM;
        }
        MR32(self + 4) = df;
        return DI_OK;
    }
    if (!strcmp(nm, "GetCapabilities")){
        uint32_t p = a[1];
        if (!p || !mem_is_committed(p + 3)) return DIERR_INVALIDPARAM;
        uint32_t sz = MR32(p); if (sz < 0x18u || sz > 0x2cu) sz = 0x2cu;
        mem_commit(p, sz); memset(g_mem + p, 0, sz); MR32(p) = sz;
        MR32(p + 0x04) = 0x00000001u;                         /* DIDC_ATTACHED */
        MR32(p + 0x08) = d->wheel ? WHEEL_DEVTYPE : PAD_DEVTYPE;
        MR32(p + 0x0c) = NAXES; MR32(p + 0x10) = NBTNS; MR32(p + 0x14) = NPOVS;
        if (dipad_ff_on()){
            MR32(p + 0x04) |= DIDC_FORCEFEEDBACK;
            if (sz >= 0x2cu){ MR32(p + 0x18) = 10000u; MR32(p + 0x1c) = 1000u; MR32(p + 0x28) = 1u; }   /* sample period, min. time resolution, driver version */
        }
        return DI_OK;
    }
    if (!strcmp(nm, "GetDeviceInfo")){
        uint32_t p = a[1];
        if (!p || !mem_is_committed(p + 3)) return DIERR_INVALIDPARAM;
        write_devinst(p, MR32(p), &s, d->wheel);
        return DI_OK;
    }
    if (!strcmp(nm, "EnumObjects")){
        uint32_t cb = a[1], ref = a[2], fl = a[3];
        if (!cb) return DIERR_INVALIDPARAM;
        static uint32_t oi = 0;
        if (!oi) oi = com_scratch(0x140);
        for (int i = 0; i < NOBJS; i++){
            uint32_t t = obj_type(i);
            if ((fl & DIDFT_TYPEMASK) && !(fl & t & DIDFT_TYPEMASK)) continue;   /* DIDFT_ALL = 0 */
            if ((fl & DIDFT_FFACTUATOR) && !(i < 2 && dipad_ff_on())) continue;
            write_objinst(oi, 0x13cu, i, s.family, d->wheel);
            uint32_t args[2] = { oi, ref };
            if (call_guest(c, cb, 2, args) == 0u) break;      /* DIENUM_STOP */
        }
        return DI_OK;
    }
    if (!strcmp(nm, "GetObjectInfo")){
        uint32_t p = a[1], obj = a[2], how = a[3];
        int i = obj_find(obj, how);
        if (i < 0) return DIERR_OBJECTNOTFOUND;
        if (!p || !mem_is_committed(p + 3)) return DIERR_INVALIDPARAM;
        write_objinst(p, MR32(p), i, s.family, d->wheel);
        return DI_OK;
    }
    if (!strcmp(nm, "SetProperty") || !strcmp(nm, "GetProperty")){
        int set = nm[0] == 'S';
        uint32_t prop = a[1], h = a[2];
        if (!h || !mem_is_committed(h + 0x17)) return DIERR_INVALIDPARAM;
        uint32_t obj = MR32(h + 8), how = MR32(h + 12);
        int lo = 0, hi = NAXES;                               /* DIPH_DEVICE: every axis */
        if (how){ int i = obj_find(obj, how);
                  if (i < 0) return DIERR_OBJECTNOTFOUND;
                  if (prop >= 4u && prop <= 6u && i >= NAXES) return DIERR_UNSUPPORTED;
                  lo = i; hi = i + 1; }
        switch (prop){
        case 1u:  if (set) d->bufsize = MR32(h + 16); else MR32(h + 16) = d->bufsize; return DI_OK;
        case 2u:  if (set) d->axismode = MR32(h + 16); else MR32(h + 16) = d->axismode; return DI_OK;
        case 4u:                                              /* RANGE */
            if (set){
                int32_t mn = (int32_t)MR32(h + 16), mx = (int32_t)MR32(h + 20);
                if (mn >= mx) return DIERR_INVALIDPARAM;
                for (int i = lo; i < hi; i++){ d->mn[i] = mn; d->mx[i] = mx; }
                if (pad_log()) fprintf(stderr, "[pad] SetProperty RANGE obj 0x%x how %u: %d..%d\n", obj, how, mn, mx);
            } else { MR32(h + 16) = (uint32_t)d->mn[lo]; MR32(h + 20) = (uint32_t)d->mx[lo]; }
            return DI_OK;
        case 5u: case 6u: {                                   /* DEADZONE, SATURATION */
            uint32_t *v = prop == 5u ? d->dz : d->sat;
            if (set){ uint32_t x = MR32(h + 16); if (x > 10000u) return DIERR_INVALIDPARAM;
                      for (int i = lo; i < hi; i++) v[i] = x;
                      if (pad_log()) fprintf(stderr, "[pad] SetProperty %s obj 0x%x: %u\n", prop == 5u ? "DEADZONE" : "SATURATION", obj, x); }
            else MR32(h + 16) = v[lo];
            return DI_OK; }
        case 9u:  if (set) d->autocenter = MR32(h + 16); else MR32(h + 16) = d->autocenter; return DI_OK;
        case 7u:                                              /* FFGAIN */
            if (!dipad_ff_on()) return DIERR_UNSUPPORTED;
            if (set){ uint32_t g = MR32(h + 16); g_ff_gain = g > 10000u ? 10000u : g;
                      if (ff_log()) fprintf(stderr, "[ff] device gain %u\n", g_ff_gain); }
            else MR32(h + 16) = g_ff_gain;
            return DI_OK;
        case 8u:  if (set || !dipad_ff_on()) return DIERR_UNSUPPORTED; MR32(h + 16) = 0; return DI_OK;   /* FFLOAD */
        case 3u:  if (!set){ MR32(h + 16) = 1; return DI_OK; } return DIERR_UNSUPPORTED;   /* GRANULARITY */
        case 15u: if (!set){ MR32(h + 16) = 0; return DI_OK; } return DIERR_UNSUPPORTED;   /* JOYSTICKID */
        case 24u: if (!set){ MR32(h + 16) = family_vidpid(s.family, d->wheel); return DI_OK; } return DIERR_UNSUPPORTED;
        default:  return set ? DI_OK : DIERR_UNSUPPORTED;
        }
    }
    if (!strcmp(nm, "GetDeviceData")){                        /* the game polls state only */
        uint32_t pn = a[3];
        if (pn && mem_is_committed(pn + 3)) MR32(pn) = 0;
        return lost ? DIERR_INPUTLOST : DI_OK;
    }
    /* force feedback: played as rumble where the platform can (ff_on);
     * otherwise answered like a device without force feedback */
    { int64_t r = ff_dev_call(c, nm, a); if (r >= 0) return (uint32_t)r; }
    if (!strcmp(nm, "Escape")) return DI_OK;
    int unsup = !strcmp(nm, "BuildActionMap") || !strcmp(nm, "SetActionMap") ||
                !strcmp(nm, "GetImageInfo") || !strcmp(nm, "RunControlPanel");
    static int nlog;
    if (pad_log() && nlog < 16){ nlog++; fprintf(stderr, "[pad] %s: %s\n", nm, unsup ? "not supported" : "nothing to do"); }
    return unsup ? DIERR_UNSUPPORTED : DI_OK;
}
