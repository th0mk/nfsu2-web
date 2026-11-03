/* dinput_pad.c -- the virtual game pad (runtime/gamepad.m) as a DirectInput8
 * gamepad device.
 *
 * How SPEED2.EXE (v1.2 NA) uses game controllers (static analysis):
 *   - IDirectInput8 #1 [0x8709ec]: EnumDevices(DI8DEVCLASS_GAMECTRL, 0x5cef00,
 *     ATTACHEDONLY) once at start-up (0x5d00b5).  The rescan every 32 frames
 *     at 0x5cfa4a is gated by [0x8364e8], which nothing ever sets.
 *   - 0x5cef00 skips two Nostromo devices, 0x5cd1f0 creates at most two
 *     devices (slots [0x86ffb4], [0x86ffb8]): c_dfDIJoystick2, cooperative
 *     level EXCLUSIVE|FOREGROUND, EnumObjects -> DIPROP_RANGE -10000..10000
 *     per axis (0x5c8660), DIDOI_FFACTUATOR objects counted in [0x86ff80].
 *   - Every frame 0x5c8700 does Poll + GetDeviceState(DIJOYSTATE2) into
 *     0x86ffc0 + slot*0x110 (DIERR_INPUTLOST releases the slot).
 *   - 0x5ca690 gives each slot a free input channel (0x874c40 + n*0x370);
 *     0x5c11a0 sets that channel's defaults: button/POV -> action tables at
 *     +0x18 (in game), +0x98 (front end), +0x118, +0x198 and the analog axes
 *     at +0x344..+0x368.  0x5b81b0 is the built-in profile lookup by
 *     guidProduct (nine Logitech/Microsoft/Saitek/... pads).
 *   - The channel bindings (+0x18 table, axes) are saved with the profile
 *     (0x5b9420, restored by 0x5b9570) keyed by the device's guidInstance.
 *
 * So the pad is a classic DIJOYSTATE2 gamepad with a STABLE guidInstance
 * (bindings persist) and our own entry in the game's profile lookup
 * (sub_005b81b0 below) giving the default layout.  Layout:
 *   axes    X/Y = left stick, Z = left trigger, Rx/Ry = right stick,
 *           Rz = right trigger (triggers rest at the minimum)
 *   POV 0   D-pad;  POV 1  left stick as four directions (menus)
 *   buttons 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 View, 7 Menu, 8 LS, 9 RS,
 *           10 LT and 11 RT pressed to the end of their travel (as a
 *           DualShock 4 under DirectInput reports L2/R2 twice), or half
 *           way when the game reads that trigger as no axis (fill_state)
 *
 * A steering wheel (PadState.wheel: the page maps the wheel to LX, the gas
 * and brake pedals to RT and LT, its buttons onto the pad's, see
 * web/src/input/wheel.ts) is offered as a driving device instead, with an
 * instance GUID of its own, so the game keeps its bindings apart from the
 * pad's and gives it the wheel layout below (g_prof_wheel).  The device type
 * matters: 0x5cd1f0 marks a driving device's slot ([0x86e5bc + slot], read by
 * 0x5c1000), and the steering then takes 0x4061c0, the wheel's position
 * straight to the car's steering angle, where a pad's or the keyboard's goes
 * through 0x416bb0, which turns the car's wheels towards it at a limited rate
 * (input controller +0x1e0, full left from straight ahead: the wheel's in the
 * first frame, the pad's within 16 standing, not within 45 at speed;
 * tests/scripts/pad_wheel_switch.txt).  The game creates its device once, so when a
 * controller of the other kind takes over (a wheel after a pad, or back), the
 * device of the old kind is let go as in hotplug mode (two polls at rest,
 * then DIERR_INPUTLOST) and the game's rescan finds the new one.
 *
 *   NFSU2_PAD=0         no game pad device (also off/no/false)
 *   NFSU2_PAD=hotplug   the device exists only while a controller is
 *                       connected (DirectInput-faithful: unplugging reports
 *                       DIERR_INPUTLOST and the game's own 32-frame rescan
 *                       is switched on).  Default: the device appears once
 *                       a controller has been connected (at start-up or
 *                       later, through the same rescan) and then stays,
 *                       reading centred while nothing is connected, so
 *                       bindings survive unplugging.
 *   NFSU2_PAD_TRIGGER_CLICK=<f>  trigger travel (0..1) at which buttons 10/11
 *                       press while the game reads the trigger as an axis
 *                       (default 0.97)
 *   NFSU2_PAD_LOG=1     log enumeration, properties and state changes
 *
 * Force feedback (rumble): the game plays DirectInput effects on a device
 * whose axes are force-feedback actuators (0x5c9150 ... 0x5ca072 create
 * spring, constant force, damper, square, sine, triangle and sawtooth
 * effects; the controller options have a Vibration switch).  When the
 * platform can drive the controller's motors (gamepad_rumble_ok: the
 * browser's dual-rumble; the Mac's gamepad.m has no motors and keeps the old
 * answer), the pad reports force feedback and plays the effects itself: every
 * frame the running ones are mixed into two motor levels (see ff_mix).
 * Those effects are made for wheels (0x5cd1f0 sorts a device by its type:
 * 0x16 driving, 0x14 joystick, 0x15 gamepad; the constant force, the springs
 * and the steering feel are a wheel's).  On a gamepad the game makes one
 * sine or square "rumble" with every level at zero and never changes it
 * (measured with NFSU2_FF_LOG in quick races: crashes, full-lock turns,
 * revving at the start), so the pad also rumbles on the player car's own
 * jolts, as the console versions do (ff_jolt).
 *   NFSU2_FF=0          no force feedback and no rumble (the pad as before;
 *                       settings.ini "rumble = off")
 *   NFSU2_FF_LOG=1      log the effects the game creates, changes, starts and
 *                       stops, and the motor levels when they change      */
#include "cpu.h"
#include "gamepad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <time.h>

uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
uint32_t com_scratch(uint32_t n);
void hfr_note_input_read(int pad);  /* framerate.c: input age */

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
    return btn[fam == PADF_XBOX ? 1 : fam == PADF_PLAYSTATION ? 2 : 0][i - NAXES - NPOVS];
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
    return btn[fam][i - NAXES - NPOVS];
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
    /* The game's own rescan every 32 frames (0x5cfa10 reads [0x8364e8]
     * every frame) finds a controller plugged in later.  Hotplug mode keeps
     * it on.  The default mode offers the device once a controller has been
     * seen and keeps it from then on (rescan off again): with no controller
     * there is no game pad, as in the original game.  A device that nothing
     * drives would leave a keyboard player stuck on the Joypad page's
     * "Press a button" prompt, which only a pad button ends. */
    static int seen = 0;
    if (mode == MODE_HOTPLUG || !seen){
        if (mem_is_committed(0x8364e8u)) MR32(0x8364e8u) = mode == MODE_HOTPLUG || !s.connected;
        if (!s.connected) return 0;
    }
    else if (mem_is_committed(0x8364e8u)) MR32(0x8364e8u) = 0;   /* the rescan after a change of kind: done */
    seen = 1;
    g_kind = wheel;
    static uint32_t ddi = 0;
    if (!ddi) ddi = com_scratch(0x260);
    write_devinst(ddi, 0x244u, &s, wheel);
    static int nlog = 0;
    if (pad_log() && nlog < 8){ nlog++;
        fprintf(stderr, "[pad] EnumDevices(type 0x%x) -> \"%s\"\n", devtype, (char *)(g_mem + ddi + 0x12c)); }
    uint32_t args[2] = { ddi, ref };
    return call_guest(c, cb, 2, args) == 0u;
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
/* The game's input channel that reads this device (0x874c40 + n*0x370, six
 * of them: +0x230 the device's slot in [0x86ffb4], +0x234 its type), or 0. */
static uint32_t dev_channel(const Dev *d){
    for (uint32_t s = 0; s < 2u; s++){
        if (!d->obj || MR32(0x86ffb4u + 4u * s) != d->obj) continue;
        for (uint32_t ch = 0x874c40u; ch < 0x876310u; ch += 0x370u){
            uint8_t t = MR8(ch + 0x234u);
            if (MR32(ch + 0x230u) == s && t >= 0x14u && t <= 0x16u) return ch;
        }
    }
    return 0;
}
/* whether the channel reads the axis at state offset ofs as the steering or
 * a pedal (0x5c1fba: +0x364..+0x367 switch the axes +0x344..+0x350 on, and
 * +0x368 with +0x367 makes one axis both pedals) */
static int chan_reads_axis(uint32_t ch, uint32_t ofs){
    if (MR8(ch + 0x364u) && MR32(ch + 0x344u) == ofs) return 1;
    if (MR8(ch + 0x368u) && MR8(ch + 0x367u)) return MR32(ch + 0x350u) == ofs;
    return (MR8(ch + 0x365u) && MR32(ch + 0x348u) == ofs) || (MR8(ch + 0x366u) && MR32(ch + 0x34cu) == ofs);
}
#define ACT_LOOK_BACK   0x09u            /* the game's action id (its keyboard map 0x7fcac0: "Look Back") */
static int g_look_back_bound;            /* the pad last read has a button on it */
int look_back_held(const PadState *s);   /* look.c */
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
    uint32_t showoff_pad(uint32_t b);                  /* showoff.c: D-pad down held while the car stands */
    uint32_t b = showoff_pad(s->buttons);
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
     * it reads as no axis is a plain button (shifting, in the PS2 layout): it
     * presses at half travel and lets go below 0.4.  Not while the Joypad
     * page waits for the steering or a pedal (+0x33c 1..4): a trigger should
     * bind there as its axis (past 5 % of its travel), and the page looks at
     * the buttons first (0x5ca720), so a fast pull could bind the button. */
    uint32_t ch = dev_channel(d);
    int axis_bind = ch && MR32(ch + 0x33cu) - 1u < 4u;
    for (int k = 0; k < 2; k++){
        float v = s->axis[k ? PAD_AXIS_RT : PAD_AXIS_LT];
        if (!ch || axis_bind || chan_reads_axis(ch, k_axis_ofs[k ? 5 : 2])) d->trig[k] = v >= click;
        else d->trig[k] = v >= (d->trig[k] ? 0.4f : 0.5f);
    }
    for (int i = 0; i < NBTNS; i++){
        int src = k_btn_src[i];
        int on = src == SRC_LT_FULL ? d->trig[0] : src == SRC_RT_FULL ? d->trig[1] : !!(b & (1u << src));
        if (on) st[0x30 + i] = 0x80;
    }
    /* the right stick straight back while driving holds the button the player has on Look Back (look.c): the
     * channel's in-game table (+0x18, by button) gives it */
    int lb = -1;
    for (int i = 0; ch && i < NBTNS && lb < 0; i++) if (MR32(ch + 0x018u + 4u * (uint32_t)i) == ACT_LOOK_BACK) lb = i;
    g_look_back_bound = lb >= 0;
    if (lb >= 0 && look_back_held(s)) st[0x30 + lb] = 0x80;
    memcpy(buf, st, size < sizeof st ? size : sizeof st);
}
/* look.c: whether the pad has a button on Look Back (else the stick turns the view round to the rear) */
int pad_look_back_bound(void){ return g_look_back_bound; }

/* ---- front-end keystrokes ----------------------------------------------------------
 * Some front-end input exists only as characters from the window procedure
 * (0x5ccf50 -> FESendKeystroke 0x55dbd0), not as DirectInput actions: text
 * fields (the profile name) take Enter and Escape that way, and the "Are you
 * sure you want to quit?" prompt opens on 'Q'/'q' (0x558b70, if
 * [0x836494] allows it).  A keyboard key produces both the action and the
 * character.  So while the game is in the front end (GameFlowManager
 * [0x8654a4] == 3) A, B and View also post Enter, Escape and Q.  (In a text
 * field View therefore types a 'q', as the Q key would.)
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
    if (on < 0) on = gamepad_env_flag("NFSU2_PAD_FE_KEYS", 1);
    uint32_t press = buttons & ~d->fe_prev, rel = d->fe_prev & ~buttons;
    d->fe_prev = buttons;
    if (!on || !mem_is_committed(0x8654a7u)) return;
    int fe = MR32(0x8654a4u) == 3u;
    fe_key(&d->fe_down[0], !!(press & 1u << PAD_A), !!(rel & 1u << PAD_A), fe, 0x0D, 0x1C, 0x0D);
    fe_key(&d->fe_down[1], !!(press & 1u << PAD_B), !!(rel & 1u << PAD_B), fe, 0x1B, 0x01, 0x1B);
    fe_key(&d->fe_down[2], !!(press & 1u << PAD_BACK), !!(rel & 1u << PAD_BACK), fe, 'Q', 0x10, 'q');
}

/* ---- the default layout ------------------------------------------------------------
 * Tables of 32 action ids: buttons 0..15, then per POV 0..3 the directions
 * down, up, left, right (0x5c1b00 tests cos/sin of the hat angle in that
 * order).  The ids are the uAppData values of the game's keyboard action
 * maps (0x7fcac0, 0x7fd7e0), i.e. bits of the channel's action mask.
 *
 * In game this is the game's own built-in layout for its known gamepads
 * (0x7fe7f8 entry 2, Logitech Dual Action / RumblePad 2: X brake, A throttle,
 * B look back, Y camera, LB nitrous, RB e-brake, LT/RT shift, Back reset,
 * Start pause, sticks hydraulics, D-pad engage/legend/map/SMS), except that
 * throttle and brake move to the analog triggers and shifting moves to the
 * buttons they free (A shift up, X shift down).  A trigger pressed to the
 * end also sends the digital throttle/brake: the analog path tops out at
 * channel byte 253 (0x7f + trunc(10000 * 0.0127)) and the game's throttle
 * curve (0x7ea004, steep above 0.95) then gives 0.958, the digital 1.0.
 * Front end: A select, B back, X customise ("C"), Y preset ("P"), LB/RB
 * L1/R1, Menu = FENG_START (the "comma" action, as on Xbox ports), View =
 * quit ("Q"; the prompt itself comes from the posted 'q', see fe_keys),
 * D-pad and left stick navigate.
 * The game's generic defaults (0x5c11a0, no profile) do not fit this pad:
 * no pause or reset, and the combined pedal axis would be Rz, our right
 * trigger, which rests at -10000 and so reads as full throttle. */
static const uint32_t g_prof_game[32] = {
    0x07, 0x09, 0x08, 0x0a, 0x06, 0x05, 0x0e, 0x0c, 0x16, 0x13, 0x02, 0x01, 0, 0, 0, 0,
    0x12, 0x0f, 0x10, 0x11,   0, 0, 0, 0,   0, 0, 0, 0,   0, 0, 0, 0 };
static const uint32_t g_prof_fe[32] = {
    0x1f, 0x22, 0x23, 0x2e, 0x26, 0x27, 0x2f, 0x24, 0, 0, 0, 0, 0, 0, 0, 0,
    0x1c, 0x1b, 0x1d, 0x1e,   0x1c, 0x1b, 0x1d, 0x1e,   0, 0, 0, 0,   0, 0, 0, 0 };
/* The wheel's, in game: the paddles shift (right up, left down), the bottom
 * face button (A, Cross) is nitrous, the left one (X, Square) the e-brake,
 * the others, the pedals pressed to the floor, the D-pad and the rest as the
 * pad's.  Its front end is the pad's. */
static const uint32_t g_prof_wheel[32] = {
    0x06, 0x09, 0x05, 0x0a, 0x08, 0x07, 0x0e, 0x0c, 0x16, 0x13, 0x02, 0x01, 0, 0, 0, 0,
    0x12, 0x0f, 0x10, 0x11,   0, 0, 0, 0,   0, 0, 0, 0,   0, 0, 0, 0 };

static void apply_profile(uint32_t ch, int wheel){
    for (int i = 0; i < 32; i++){
        MR32(ch + 0x018 + 4u * (uint32_t)i) = (wheel ? g_prof_wheel : g_prof_game)[i];
        MR32(ch + 0x098 + 4u * (uint32_t)i) = g_prof_fe[i];
        MR32(ch + 0x118 + 4u * (uint32_t)i) = 0;            /* secondary tables unused */
        MR32(ch + 0x198 + 4u * (uint32_t)i) = 0;
    }
    /* analog: steer = X, throttle = Rz (right trigger), brake = Z (left
     * trigger), as separate pedals (what the joypad screen stores when two
     * different axes are bound; 0x5b8250).  The channel turns them into
     * +0x12 steer and +0x10 throttle / +0x11 brake, 0..254 (0x5c1fba). */
    MR32(ch + 0x344) = 0x00;  MR32(ch + 0x354) = 1;          /* steer: offset, sign */
    MR32(ch + 0x348) = 0x14;  MR32(ch + 0x358) = 1;          /* throttle */
    MR32(ch + 0x34c) = 0x08;  MR32(ch + 0x35c) = 1;          /* brake */
    MR32(ch + 0x350) = 0x14;  MR32(ch + 0x360) = 1;          /* combined pedal (unused) */
    MR8(ch + 0x364) = 1; MR8(ch + 0x365) = 1; MR8(ch + 0x366) = 1;
    MR8(ch + 0x367) = 0; MR8(ch + 0x368) = 0;
    if (pad_log()) fprintf(stderr, "[pad] default %s layout applied to input channel 0x%08x\n", wheel ? "wheel" : "pad", ch);
}

/* ---- the bindings over an unplug -------------------------------------------------
 * The player's bindings live in the device's input channel: the in-game
 * table (+0x18, 32 actions) and the analog ones (+0x344 .. +0x368), what
 * the profile saves per guidInstance (0x5b9420) and gives back when it loads
 * (0x5b9570), and nothing else.  When the game lets a device go (DIERR_INPUTLOST,
 * 0x5c883f clears its slot [0x86ffb4 + 4 slot]), the next frame frees its
 * channel (0x5c0f90: +0x230 = -1), and a device made later takes a free
 * channel with the defaults (0x5ca690 -> 0x5c14f0 -> 0x5c11a0 -> the lookup
 * below): in hotplug mode a controller unplugged and plugged in again (an
 * Xbox pad that switches itself off when left alone, and on again) came back
 * with the default layout until the profile was loaded again, a restart (a
 * player's report: Shift Up on RB and Down on LB, back on A and X).  So the
 * bindings of a pad (or wheel) unplugged are kept as the game lets it go,
 * and the next device of its kind the game sets up gets them back, once:
 * the Controls page's reset to default still resets.  A device let go for a
 * controller of the other kind keeps the old way (its successor gets the
 * defaults of its own kind). */
#define G_DEVSLOT       0x0086ffb4u      /* the game's 2 device slots */
#define G_CHANNELS      0x00874c40u      /* its 6 input channels, CH_SIZE each */
#define CH_SIZE         0x370u
#define CH_SLOT         0x230u           /* the device slot it reads (-1 free) */
#define CH_TYPE         0x234u           /* 0x14 joystick, 0x15 gamepad, 0x16 driving (0x5c14f0) */
#define CH_GAME         0x018u
#define CH_GAME_N       0x80u
#define CH_AXES         0x344u
#define CH_AXES_N       0x25u
static struct { int have; uint8_t game[CH_GAME_N], axes[CH_AXES_N]; } g_kept[2];   /* [0] the pad, [1] the wheel */
static uint32_t channel_of(uint32_t obj){
    for (uint32_t i = 0; i < 2u; i++){
        if (MR32(G_DEVSLOT + 4u * i) != obj) continue;
        for (uint32_t n = 0; n < 6u; n++){
            uint32_t ch = G_CHANNELS + CH_SIZE * n, t = MR32(ch + CH_TYPE);
            if (MR32(ch + CH_SLOT) == i && t >= 0x14u && t <= 0x16u) return ch;
        }
    }
    return 0;
}
static void keep_bindings(uint32_t obj, int wheel){
    uint32_t ch = channel_of(obj);
    if (!ch) return;
    memcpy(g_kept[wheel].game, g_mem + ch + CH_GAME, CH_GAME_N);
    memcpy(g_kept[wheel].axes, g_mem + ch + CH_AXES, CH_AXES_N);
    g_kept[wheel].have = 1;
    fprintf(stderr, "[pad] the %s is unplugged: its bindings are kept for when it is back\n", wheel ? "wheel" : "pad");
}
static void bindings_back(uint32_t ch, int wheel){
    if (!g_kept[wheel].have) return;
    g_kept[wheel].have = 0;
    memcpy(g_mem + ch + CH_GAME, g_kept[wheel].game, CH_GAME_N);
    memcpy(g_mem + ch + CH_AXES, g_kept[wheel].axes, CH_AXES_N);
    fprintf(stderr, "[pad] the %s is back: its bindings from before the unplug (input channel 0x%08x)\n", wheel ? "wheel" : "pad", ch);
}

/* 0x5b81b0(devinfo, tableA, tableB): the game's built-in profile lookup,
 * called from the channel set-up 0x5c11a0 (device attach, and "reset to
 * default" in the controls screen).  cdecl, returns al = 1 when found. */
void sub_005b81b0_orig(CPU *c);
void sub_005b81b0(CPU *c){
    uint32_t ddi = MR32(c->esp + 4u), ta = MR32(c->esp + 8u);
    int ours = pad_mode() != MODE_OFF && ddi ? dipad_guid(ddi + 4u) : 0;
    if (ours && ta > 0x18u){
        apply_profile(ta - 0x18u, ours == 2);
        bindings_back(ta - 0x18u, ours == 2);
        c->eax = (c->eax & 0xFFFFFF00u) | 1u;
        POP32(c->eip);
        return;
    }
    sub_005b81b0_orig(c);
}

/* ---- binding on the Joypad page -------------------------------------------------------
 * The page has two columns.  For the steering and the pedals (actions 1..4)
 * an axis, while one is on, shows in the first column and the action's
 * button in the second (0x5b8a90).  Binding a button (0x5b8460) or a hat
 * direction (0x5c1720) replaces the action's other buttons but leaves its
 * axis on, and nothing on the page turns an axis off: with the gas and brake
 * moved to buttons and the triggers to shifting (the PS2 layout), a trigger
 * still accelerated or braked as it shifted.  Here a button or hat direction
 * bound to the throttle or the brake in the first column takes the axis's
 * place, as the column shows: the axis goes off (and pulling a trigger there
 * binds it again).  The steering keeps its axis (left and right are the one
 * stick axis), and so does one axis bound to both pedals (+0x368).  Channel
 * (thiscall ecx): +0x33c the action being bound, +0x340 the column (0 the
 * first), +0x365/+0x366 the throttle/brake axis on; saved with the profile. */
static void bind_takes_axis(uint32_t ch){
    uint32_t act = MR32(ch + 0x33cu);
    if ((act != 1u && act != 2u) || MR32(ch + 0x340u) != 0u || MR8(ch + 0x368u) || !MR8(ch + 0x364u + act)) return;
    MR8(ch + 0x364u + act) = 0;
    if (pad_log()) fprintf(stderr, "[pad] %s bound to a button: its axis is off\n", act == 1u ? "throttle" : "brake");
}
void sub_005b8460_orig(CPU *c);
void sub_005b8460(CPU *c){ bind_takes_axis(c->ecx); sub_005b8460_orig(c); }
void sub_005c1720_orig(CPU *c);
void sub_005c1720(CPU *c){ bind_takes_axis(c->ecx); sub_005c1720_orig(c); }

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

/* The Controls page lists the devices it found when it opened (0x4ace00).
 * Losing the pad while it is open crashes the game as soon as one of the
 * pad's bindings is edited (Enter: 0x4f9340 reads a NULL object at +0x64),
 * so there an unplugged pad reads at rest and is lost once the page is
 * left. */
const char *autotest_screen(void);              /* autotest.c */
static int on_controls_page(void){
    const char *sc = autotest_screen();
    return sc && !strcasecmp(sc, "UI_Options_PC_Controller.fng");
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
/* The player car's jolts: impacts and hard landings.  car+0x70 is the
 * velocity (m/s), [0x865154] this frame's world step (s; 0 while the world
 * stands), [0x8654a4] 6 in the world.  A change of velocity over the last
 * 30 ms of world time (the same window at 60 and 120 fps: a crash may take
 * two steps at 120) that driving cannot make (more than 40 m/s^2, about 4 g)
 * is a hit: its excess sets the level (4 m/s and more: full), on the strong
 * motor and a little of it on the weak one, decaying with a time constant of
 * 0.12 s.  A jump of more than 60 m/s is the game putting the car back (a
 * reset, a teleport), not a hit, and so is everything in the car's first
 * second (the race intro drops it onto the grid: 8.8 m/s, measured).
 * Measured in a quick race at 60 fps: 6.4 m/s for a wall at 23 m/s, 16 m/s
 * for a crash, 1.2 to 5 m/s scraping along a barrier. */
static float g_jolt;                    /* 0..1, decaying */
static uint32_t g_jolt_car;
#define JOLT_HIST 32
static struct { float v[3], t; } g_jh[JOLT_HIST];   /* velocity at world time t (s), newest at g_jhn - 1 */
static int g_jhn;
static float g_jolt_age, g_jolt_logt;   /* world seconds since this car appeared; since the last speed line */
static void ff_jolt(void){
    uint32_t pl = mem_is_committed(0x8900acu + 3u) ? MR32(0x8900acu) : 0u;
    uint32_t car = pl && mem_is_committed(pl + 7u) ? MR32(pl + 4u) : 0u;
    float dt = MRF32(0x865154u);
    if (MR32(0x8654a4u) != 6u || !car || !mem_is_committed(car + 0x7bu)){ g_jolt_car = 0; g_jolt = 0; return; }
    float v[3] = { MRF32(car + 0x70u), MRF32(car + 0x74u), MRF32(car + 0x78u) };
    if (!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2])) return;
    if (!(dt > 0.f && dt < 0.25f)) return;              /* the world stands (pause, a menu over it) */
    g_jolt_age = car == g_jolt_car ? g_jolt_age + dt : 0.f;
    if (ff_log() && (g_jolt_logt += dt) >= 2.f){
        g_jolt_logt = 0.f;
        fprintf(stderr, "[ff] car speed %.1f m/s\n", (double)sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
    }
    if (car != g_jolt_car) g_jhn = 0;
    float now = g_jolt_age;
    if (car == g_jolt_car && g_jolt_age > 1.f && g_jhn > 0){
        /* the oldest sample still inside the 30 ms window (or the oldest kept) */
        int k = g_jhn - 1;
        while (k > 0 && now - g_jh[(k - 1) % JOLT_HIST].t <= 0.030f && g_jhn - k < JOLT_HIST - 1) k--;
        const float *o = g_jh[k % JOLT_HIST].v;
        float span = now - g_jh[k % JOLT_HIST].t;
        float dx = v[0] - o[0], dy = v[1] - o[1], dz = v[2] - o[2];
        float dv = sqrtf(dx * dx + dy * dy + dz * dz);
        float hit = dv - 40.f * span;
        if (dv < 60.f && hit > 0.f){
            float lvl = hit / 4.f;
            if (lvl > 1.f) lvl = 1.f;
            if (lvl > g_jolt + 0.05f && ff_log()) fprintf(stderr, "[ff] jolt %.2f m/s in %.1f ms: level %.2f\n", (double)dv, (double)span * 1000.0, (double)lvl);
            if (lvl > g_jolt) g_jolt = lvl;
        }
    }
    { int n = g_jhn % JOLT_HIST; g_jh[n].v[0] = v[0]; g_jh[n].v[1] = v[1]; g_jh[n].v[2] = v[2]; g_jh[n].t = now; g_jhn++; }
    g_jolt *= expf(-dt / 0.12f);
    if (g_jolt < 0.01f) g_jolt = 0.f;
    g_jolt_car = car;
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
    ff_jolt();
    strong += g_jolt; weak += g_jolt * 0.4f;
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
     * pad, or back): this device makes way for one of that kind */
    int other = s.connected && !!s.wheel != d->wheel;
    int lost = ((pad_mode() == MODE_HOTPLUG && !s.connected) || other) && !on_controls_page();
    (void)nargs;
    if (!strcmp(nm, "GetDeviceState")){
        uint32_t cb = a[1], p = a[2];
        hfr_note_input_read(1);
        /* Unplugged (hotplug mode), or the other kind: answer two polls with
         * the pad at rest before DIERR_INPUTLOST.  The game drops the
         * device's channel with its last values, which then stay applied
         * (steering, throttle, held buttons) until its next 32-frame rescan,
         * about 0.3 s, which then finds the device of the new kind. */
        if (lost && d->rest >= 2){
            if (other && !d->gone){
                if (mem_is_committed(0x8364e8u)) MR32(0x8364e8u) = 1;
                fprintf(stderr, "[pad] %s drives the pad: the game's %s device makes way for a %s\n",
                        s.name, d->wheel ? "wheel" : "pad", s.wheel ? "wheel" : "pad");
            }
            else if (!d->gone) keep_bindings(self, d->wheel);     /* unplugged: the next one of its kind gets them */
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
    if (!strcmp(nm, "BuildActionMap") || !strcmp(nm, "SetActionMap") ||
        !strcmp(nm, "GetImageInfo") || !strcmp(nm, "RunControlPanel")) return DIERR_UNSUPPORTED;
    return DI_OK;
}

/* ---- test hook: the player's input controller ----------------------------------
 * 0x407750 is the player's steering joy-event handler (cdecl: port, id,
 * value, ctx), registered by 0x41e060 for JOY_EVENT_STEER/_STEER_ANALOG.
 * ctx is the object the car reads its input from (0x407750, 0x416de0,
 * 0x407eb0, 0x407f40): +0x2c8 steer (-1..1, positive = left), +0x2d4
 * throttle, +0x2dc brake, +0x2ec e-brake (0..1).  Only observed here, so
 * the autotest script can peek it as "@in+<offset>". */
static uint32_t g_in_ctx;
uint32_t dipad_input_ctx(void){ return g_in_ctx; }
void sub_00407750_orig(CPU *c);
void sub_00407750(CPU *c){
    uint32_t ctx = MR32(c->esp + 0x10u);
    if (ctx != g_in_ctx){
        g_in_ctx = ctx;
        if (pad_log()) fprintf(stderr, "[pad] player input controller at 0x%08x\n", ctx);
    }
    sub_00407750_orig(c);
}

/* ---- the keyboard's steering next to a wheel ----------------------------------
 * 0x5c1000 (cdecl: port) tells whether the port's device is a driving device;
 * the driver's update (0x41dfc0, the call returning to 0x41dfea) then steers
 * with 0x4061c0, the steering straight to the car's angle, instead of
 * 0x416bb0, which turns the wheels towards it at a rate and a lock that fall
 * with the speed.  The game asks the port, not where the steering came from,
 * and the keyboard's steering is the same event as the wheel's (0x407750: a
 * key gives 0x00 or 0xFF), so with a wheel connected, resting or not, a key
 * steered to full lock in the first frame at any speed (a player's report:
 * "the car can now steer to full lock at speed").  So for the driver's update
 * the wheel is a driving device only while it is turned from the centre;
 * resting, the keyboard (or nothing) steers as without a wheel.  ZF as the
 * game's own answer leaves it: the caller tests it.
 * tests/scripts/kb_steer_wheel.txt. */
#define WHEEL_STEERS 0.02f               /* |LX| from which the wheel is turned */
void sub_005c1000_orig(CPU *c);
void sub_005c1000(CPU *c){
    uint32_t ret = MR32(c->esp);
    sub_005c1000_orig(c);
    if (ret != 0x0041dfeau || !(c->eax & 0xffu)) return;
    PadState s; gamepad_read(&s);
    if (s.connected && s.wheel && fabsf(s.axis[PAD_AXIS_LX]) >= WHEEL_STEERS) return;
    c->eax &= ~0xffu;
    c->zf = 1;
}

/* ---- the right stick turns the car in the garage and the shops ----------------
 * The front end's car viewer (the garage, the shops, the car lots) runs its
 * frame from its message handler (0x4d8d20, message 0xc98356ba): the update
 * (0x4d82d0), then the mouse (0x4a5d90, thiscall: the viewer).  While the
 * middle button is held (the mouse at 0x8763d0: +0x27 down, +0x29 let go this
 * frame) the drag sets the viewer's turn speed +0x248 = dx / 2 and its tilt
 * speed +0x24c = dy / 2, and stops the showcase that turns the car once
 * nothing was pressed for a while ([0x8363fc] = 0, its idle clock [0x837aa0]
 * = the game's time [0x8651ac]).  Letting go sets both back to 0, and so does
 * a GenericDialog.fng over the viewer.  The next update turns the car by
 * +0x248 x dt x 60 degrees (+0x274) and moves its camera up or down by
 * 0.1 x +0x24c x dt x 60 (0x44ce00: camera +0x144, through 0x4454e0; from
 * 1.2 to 5.2).  The consoles' right stick sets the same two, -1 to 1
 * (0x4a5740: actions 0x42 to 0x47), which no binding of the PC's front-end
 * pad table reaches.  So here, after the game's own and unless the middle
 * button is held, the controller's right stick out of its dead zone
 * (gamepad_read) sets them at the consoles' scale: full stick turns the car 60
 * degrees a second, about a fast drag.  Stick right turns the view right, as
 * photo mode's right stick turns its orbit camera (photo.c) and as a drag to
 * the right turns it here: the car's near side moves right, the garage behind
 * it left.  (The consoles' axis, 0x44, turns it the other way.)  Stick down
 * looks further down at the car: the camera
 * rises, as photo mode's and a drag down here do.  Back at rest both go to 0
 * once.  A wheel's and an RC radio's right stick is always at rest
 * (web/src/input/wheel.ts).  The angle starts over on every screen, as the
 * game has it. */
#define VW_CAMERA    0x23cu        /* the viewer's camera (+0xa8 its height) */
#define VW_TURN      0x248u        /* float: the viewer's turn speed (60 degrees a second at 1) */
#define VW_TILT      0x24cu        /* float: its camera's tilt speed */
#define VW_ANGLE     0x274u        /* float: the car's angle, degrees */
#define CAM_HEIGHT   0xa8u
#define G_FE_BUSY    0x00838578u   /* the front end takes no input now (0x510c30) */
#define G_MOUSE_MID  0x008763f7u   /* byte: the mouse's middle button is down */
#define G_SHOWCASE   0x008363fcu   /* byte: the showcase turns the car */
#define G_VIEW_IDLE  0x00837aa0u   /* when the viewer was last turned (the showcase starts a while after) */
#define G_FE_TIME    0x008651acu   /* the game's time */
int career_fe_active(const char *name);
void sub_004a5d90_orig(CPU *c);
void sub_004a5d90(CPU *c){
    static uint32_t turning;       /* the viewer the stick turns, 0 none */
    uint32_t v = c->ecx;
    sub_004a5d90_orig(c);
    if (MR8(G_MOUSE_MID)){ turning = 0; return; }             /* the drag's */
    PadState s;
    float x = 0.f, y = 0.f;
    if (pad_mode() != MODE_OFF && gamepad_read(&s) && !s.wheel){ x = s.axis[PAD_AXIS_RX]; y = s.axis[PAD_AXIS_RY]; }
    if ((x == 0.f && y == 0.f) || MR32(G_FE_BUSY) || career_fe_active("GenericDialog.fng")){
        if (turning == v){
            MRF32(v + VW_TURN) = 0.f; MRF32(v + VW_TILT) = 0.f;
            uint32_t cam = MR32(v + VW_CAMERA);
            if (pad_log()) fprintf(stderr, "[pad] the right stick let go: the car at %.0f degrees, the camera at %.2f\n",
                                   MRF32(v + VW_ANGLE), cam && mem_is_committed(cam + CAM_HEIGHT + 3u) ? MRF32(cam + CAM_HEIGHT) : -1.f);
        }
        turning = 0;
        return;
    }
    MRF32(v + VW_TURN) = x;
    MRF32(v + VW_TILT) = y;
    MR8(G_SHOWCASE) = 0;
    MR32(G_VIEW_IDLE) = MR32(G_FE_TIME);
    if (!turning && pad_log()) fprintf(stderr, "[pad] the right stick turns the car (viewer 0x%08x, the car at %.0f degrees)\n", v, MRF32(v + VW_ANGLE));
    turning = v;
}
