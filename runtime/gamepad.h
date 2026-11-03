/* gamepad.h -- the virtual game pad between GameController.framework
 * (runtime/gamepad.m) and the DirectInput8 gamepad device the game sees
 * (runtime/dinput_pad.c).
 *
 * Exactly one virtual pad exists.  The first connected controller with an
 * extended gamepad profile (Xbox, DualSense, DualShock 4, Switch Pro, MFi)
 * drives it; the autotest script can drive it instead (gamepad_synth_*), so
 * a scripted run takes the same path as a real controller from here on.
 * Buttons are positional, Xbox names: A = bottom face button (Cross on a
 * PlayStation pad), B = right, X = left, Y = top.                         */
#ifndef GAMEPAD_H
#define GAMEPAD_H
#include <stdint.h>

enum {
    PAD_A, PAD_B, PAD_X, PAD_Y, PAD_LB, PAD_RB, PAD_BACK, PAD_START,
    PAD_LS, PAD_RS, PAD_HOME, PAD_DUP, PAD_DDOWN, PAD_DLEFT, PAD_DRIGHT,
    PAD_NBUTTONS
};
enum { PAD_AXIS_LX, PAD_AXIS_LY, PAD_AXIS_RX, PAD_AXIS_RY, PAD_AXIS_LT, PAD_AXIS_RT, PAD_NAXES };
/* controller family: picks the button names and the reported VID/PID */
enum { PADF_GENERIC, PADF_XBOX, PADF_PLAYSTATION, PADF_NINTENDO };

typedef struct {
    int      connected;          /* a controller (or the script) drives the pad */
    uint32_t buttons;            /* 1u << PAD_x */
    float    axis[PAD_NAXES];    /* sticks -1..1 (+x right, +y down), triggers 0..1 */
    int      family;             /* PADF_x */
    int      synthetic;          /* driven by the input script */
    int      wheel;              /* a steering wheel (the browser's): LX the wheel, LT and RT
                                    the brake and gas pedals, the buttons as the page put them;
                                    dinput_pad.c then offers the game a driving device */
    char     name[64];           /* "Xbox Wireless Controller", ... */
} PadState;

/* An on/off option: a number is on unless 0; off/no/false and on/yes/true
 * (any other word) as written; unset or empty gives dflt.  settings.ini
 * passes its values through verbatim. */
int  gamepad_env_flag(const char *nm, int dflt);
/* Starts GameController discovery (idempotent; any thread). */
void gamepad_init(void);
/* Snapshot of the virtual pad after dead zones; returns s->connected. */
int  gamepad_read(PadState *s);
#ifdef __wasm__
/* Shared-engine local seats, 0..5; independent of the legacy auto-selected
 * pad. Returns neutral input on disconnect or an inconsistent mailbox read. */
int gamepad_read_local(unsigned seat, PadState *s);
uint32_t gamepad_local_join_stamp(unsigned seat);
/* Autotest only: control one seat without changing any other seat. key -1
 * connects/disconnects; buttons precede axes (PAD_NBUTTONS + PAD_AXIS_*). */
void gamepad_synth_local(unsigned seat, int key, float value);
#endif

/* autotest: a synthetic controller (connects on first use) */
void gamepad_synth_connect(int on);
void gamepad_synth_button(int b, int down);
void gamepad_synth_axis(int a, float v);
int  gamepad_button_by_name(const char *nm);   /* "A", "LB", "START", "DUP", ... or -1 */
int  gamepad_axis_by_name(const char *nm);     /* "LX" .. "RT" or -1 */

/* Rumble (dinput_pad.c plays the game's force-feedback effects on it):
 * 1 when the controller driving the pad has motors the platform can drive.
 * The browser's input_web.c drives Chrome's dual-rumble; gamepad.m drives no
 * motors, and dinput_pad.c's weak defaults (0, nothing) stand in for it. */
int  gamepad_rumble_ok(void);
/* the two motor levels, 0..1 (strong = the low-frequency motor), every frame
 * while the game polls the pad; the platform stops the motors by itself when
 * the updates stop (a paused or parked game) */
void gamepad_rumble(float strong, float weak);
#endif
