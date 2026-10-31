/* pad_mw.c -- Most Wanted's default controller layout (TRACKER M6-37, M6-38; PLAN D-65): the modern one of
 * Underground 2's pad (runtime/pad_ug2.c) for any controller the page gives the game, a steering wheel's pedals
 * included.
 *
 * How speed.exe (v1.3) picks a joystick's bindings (GAME.md 13.1, static and the pad_binds runs): the binding table
 * 0x91F418 holds 76 rows of 0x34 bytes, each a name and four bindings (type, device or key, code), the keyboard's at
 * +0x04 and +0x10, the joystick's at +0x1C and +0x28.  0x63CD80 (thiscall, the PC input device) hashes the product
 * name of the first joystick ([0x91F150] +0x114, 0x460BC0) and writes both joystick bindings of rows 0..23
 * (GAME_ACTION_GAS to HUDACTION_SKIPNIS) from a layout per known product; for any other name (every browser
 * controller) a layout of its own: X gas, B brake, A handbrake, Y Speedbreaker, LT nitrous, LB look back, RB
 * respawn, RT pause (so a wheel's gas pedal, the page's RT, paused the game), no gears, no camera.  It runs from the
 * input device's set-up (0x63F850, method 3 of vtable 0x8A7B9C, which tail-jumps into it: a lifted tail jump runs
 * the original, so the set-up is overridden too), from the profile load when the saved joystick's name hash
 * (0x634DA0(3)) is not the one connected (0x56E27D) and from 0x51D702.  The profile keeps rows 0..18 (0x30 bytes
 * each, after the name) with that hash (0x56E200, from UserProfile's load 0x5ACB70), so a player's own bindings for
 * the same controller come back as they were.
 *
 * Here, after the game's own lookup, the rows D-65 names get the modern layout (the buttons in the order game_mw.c
 * pad_buttons gives the device: 0 X, 1 A, 2 B, 3 Y, 4 LB, 5 RB, 6 LT, 7 RT, 8 View, 9 Menu, 10 LS, 11 RS):
 *   RT gas, LT brake and reverse (the triggers' axes, read from the centre: game_mw.c pad_trigger_centre), the left
 *   stick steers, LB nitrous, RB handbrake, A shift up, X shift down, Y camera, B look back, View respawn, Menu
 *   pause, the right stick pressed Speedbreaker.
 * A steering wheel (dinput_pad.c's driving device) shifts with its paddles (the page's RB up, LB down), its bottom
 * face button (A, Cross) is nitrous and its left one (X, Square) the handbrake, the rest as the pad's: UG2's wheel
 * layout.  The other rows (the D-pad's HUD actions, the menus' directions, skipping a cutscene) stay the game's.
 * A profile saved with the game's own layout for this controller (a build before this one) gets the modern one at
 * its load; one with the player's own bindings keeps them.  The axis types (0x628940, centre 32767.5): 4/5 the
 * X, Y or Z axis (code 0..2) below or above its centre, 6/7 Rx, Ry or Rz, 8/9 the sliders. */
#include "cpu.h"
#include <stdio.h>
#include <string.h>

int dipad_kind(uint32_t obj);           /* dinput_pad.c: 1 the pad device, 2 the wheel, 0 neither */
int dipad_log(void);

void sub_0063cd80_orig(CPU *c);
void sub_0063f850_orig(CPU *c);
void sub_0056e200_orig(CPU *c);

#define BINDS      0x0091f418u          /* the binding table: 76 rows of 0x34 bytes */
#define ROW        0x34u
#define JOY_SLOTS  0x1cu                /* the joystick's two bindings, 6 dwords */
#define ROWS_LOOKUP 24u                 /* 0x63CD80 writes rows 0..23 */
#define ROWS_SAVED  19u                 /* the profile keeps rows 0..18 */
#define JOYSTICK0  0x0091f150u          /* the first joystick object */
#define JOY_DEVICE 0x468u               /* its IDirectInputDevice8 (0x7F277B: GetCapabilities through it) */

enum { GAS, BRAKE, STEERLEFT, STEERRIGHT, HANDBRAKE, GAMEBREAKER, NOS, SHIFTDOWN, SHIFTUP, RESET,
       CAM_CHANGE = 15, LOOKBACK = 16, PAUSEREQUEST = 18 };
enum { B_X, B_A, B_B, B_Y, B_LB, B_RB, B_LT, B_RT, B_VIEW, B_MENU, B_LS, B_RS };
#define BUTTON 2u
#define AXIS_LO 4u                      /* X/Y/Z below the centre */
#define AXIS_HI 5u                      /* X/Y/Z above it */
#define RAXIS_HI 7u                     /* Rx/Ry/Rz above it */

typedef struct { int row; uint32_t type, code; } Bind;
static const Bind k_pad[] = {
    { GAS, RAXIS_HI, 2 },               /* Rz: the right trigger */
    { BRAKE, AXIS_HI, 2 },              /* Z: the left trigger */
    { STEERLEFT, AXIS_LO, 0 }, { STEERRIGHT, AXIS_HI, 0 },
    { HANDBRAKE, BUTTON, B_RB }, { GAMEBREAKER, BUTTON, B_RS }, { NOS, BUTTON, B_LB },
    { SHIFTDOWN, BUTTON, B_X }, { SHIFTUP, BUTTON, B_A }, { RESET, BUTTON, B_VIEW },
    { CAM_CHANGE, BUTTON, B_Y }, { LOOKBACK, BUTTON, B_B }, { PAUSEREQUEST, BUTTON, B_MENU },
};
static const Bind k_wheel[] = {         /* where the wheel differs */
    { HANDBRAKE, BUTTON, B_X }, { NOS, BUTTON, B_A }, { SHIFTDOWN, BUTTON, B_LB }, { SHIFTUP, BUTTON, B_RB },
};

/* the game's own joystick bindings of rows 0..23, as its lookup last wrote them */
static uint32_t g_game[ROWS_LOOKUP][6];
static int g_have_game;

/* the first joystick's kind: 1 our pad, 2 our wheel, 0 none (or not ours) */
static int joystick_kind(void){
    uint32_t joy = mem_is_committed(JOYSTICK0 + 3u) ? MR32(JOYSTICK0) : 0u;
    if (!joy || !mem_is_committed(joy + JOY_DEVICE + 3u)) return 0;
    return dipad_kind(MR32(joy + JOY_DEVICE));
}
static void set_bind(int row, uint32_t type, uint32_t code){
    uint32_t p = BINDS + (uint32_t)row * ROW + JOY_SLOTS;
    MR32(p) = type; MR32(p + 4) = 0; MR32(p + 8) = code;          /* (type, joystick 0, code) */
    MR32(p + 12) = 0; MR32(p + 16) = 0; MR32(p + 20) = 0;         /* no second binding */
}
static void write_layout(int kind, const char *why){
    for (size_t i = 0; i < sizeof k_pad / sizeof k_pad[0]; i++) set_bind(k_pad[i].row, k_pad[i].type, k_pad[i].code);
    if (kind == 2)
        for (size_t i = 0; i < sizeof k_wheel / sizeof k_wheel[0]; i++) set_bind(k_wheel[i].row, k_wheel[i].type, k_wheel[i].code);
    if (dipad_log()) fprintf(stderr, "[pad] Most Wanted's default %s layout (%s)\n", kind == 2 ? "wheel" : "pad", why);
}
/* after the game's lookup: keep what it wrote, then write ours */
static void after_lookup(const char *why){
    int kind = joystick_kind();
    if (!kind || !mem_is_committed(BINDS + ROWS_LOOKUP * ROW - 1u)) return;
    for (uint32_t r = 0; r < ROWS_LOOKUP; r++)
        for (uint32_t k = 0; k < 6; k++) g_game[r][k] = MR32(BINDS + r * ROW + JOY_SLOTS + 4u * k);
    g_have_game = 1;
    write_layout(kind, why);
}

void sub_0063cd80(CPU *c){
    sub_0063cd80_orig(c);
    after_lookup("the game's lookup");
}
/* the input device's set-up: it returns at once when it has run ([this+0x24] set), else ends in the lookup */
void sub_0063f850(CPU *c){
    uint32_t self = c->ecx;
    int runs = self && mem_is_committed(self + 0x27u) && MR32(self + 0x24u) == 0u;
    sub_0063f850_orig(c);
    if (runs) after_lookup("the input set-up");
}
/* the profile's bindings: one saved with the game's own layout for this controller gets ours */
void sub_0056e200(CPU *c){
    sub_0056e200_orig(c);
    int kind = joystick_kind();
    if (!kind || !g_have_game) return;
    for (uint32_t r = 0; r < ROWS_SAVED; r++)
        for (uint32_t k = 0; k < 6; k++)
            if (MR32(BINDS + r * ROW + JOY_SLOTS + 4u * k) != g_game[r][k]) return;
    write_layout(kind, "a profile with the game's own layout");
}
