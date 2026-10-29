/* pad_ug2.c -- Underground 2's side of the game pad (runtime/dinput_pad.c,
 * the DirectInput 8 joystick device of every game): what SPEED2.EXE needs of
 * it beyond DirectInput, reached through runtime/game.h (game_ug2.c's table)
 * and the game's own routines overridden here.
 *
 * How SPEED2.EXE (v1.2 NA) uses game controllers (static analysis):
 *   - IDirectInput8 #1 [0x8709ec]: EnumDevices(DI8DEVCLASS_GAMECTRL, 0x5cef00,
 *     ATTACHEDONLY) once at start-up (0x5d00b5).  The rescan every 32 frames
 *     at 0x5cfa4a is gated by [0x8364e8], which nothing ever sets
 *     (ug2_pad_rescan sets it).
 *   - 0x5cef00 skips two Nostromo devices, 0x5cd1f0 creates at most two
 *     devices (slots [0x86ffb4], [0x86ffb8]): c_dfDIJoystick2, cooperative
 *     level EXCLUSIVE|FOREGROUND, EnumObjects -> DIPROP_RANGE -10000..10000
 *     per axis (0x5c8660), DIDOI_FFACTUATOR objects counted in [0x86ff80].
 *   - Every frame 0x5c8700 does Poll + GetDeviceState(DIJOYSTATE2) into
 *     0x86ffc0 + slot*0x110 (DIERR_INPUTLOST releases the slot).  When Poll
 *     fails it just re-acquires and returns, so only a failing GetDeviceState
 *     reaches the release path (0x5c883f): dinput_pad.c fails that alone.
 *   - 0x5ca690 gives each slot a free input channel (0x874c40 + n*0x370);
 *     0x5c11a0 sets that channel's defaults: button/POV -> action tables at
 *     +0x18 (in game), +0x98 (front end), +0x118, +0x198 and the analog axes
 *     at +0x344..+0x368.  0x5b81b0 is the built-in profile lookup by
 *     guidProduct (nine Logitech/Microsoft/Saitek/... pads).
 *   - The channel bindings (+0x18 table, axes) are saved with the profile
 *     (0x5b9420, restored by 0x5b9570) keyed by the device's guidInstance.
 * So the pad gets our own entry in the game's profile lookup (sub_005b81b0
 * below), which gives the default layout.  The VID/PID of dinput_pad.c's
 * product GUIDs (family_vidpid) collide with none of the game's own lists
 * (0x7fe7f8, 0x800588, 0x5cef00).
 *
 * The device type matters: 0x5cd1f0 marks a driving device's slot
 * ([0x86e5bc + slot], read by 0x5c1000), and the steering then takes
 * 0x4061c0, the wheel's position straight to the car's steering angle, where
 * a pad's or the keyboard's goes through 0x416bb0, which turns the car's
 * wheels towards it at a limited rate (input controller +0x1e0, full left
 * from straight ahead: the wheel's in the first frame, the pad's within 16
 * standing, not within 45 at speed; tests/scripts/pad_wheel_switch.txt).  The
 * game creates its device once, so when a controller of the other kind takes
 * over, dinput_pad.c lets the old device go and the rescan finds the new one.
 *
 * Force feedback: the game plays DirectInput effects on a device whose axes
 * are force-feedback actuators (0x5c9150 ... 0x5ca072 create spring,
 * constant force, damper, square, sine, triangle and sawtooth effects; the
 * controller options have a Vibration switch).  Those effects are made for
 * wheels (0x5cd1f0 sorts a device by its type: 0x16 driving, 0x14 joystick,
 * 0x15 gamepad; the constant force, the springs and the steering feel are a
 * wheel's).  On a gamepad the game makes one sine or square "rumble" with
 * every level at zero and never changes it (measured with NFSU2_FF_LOG in
 * quick races: crashes, full-lock turns, revving at the start), so the pad
 * also rumbles on the player car's own jolts (ug2_pad_jolt).  The game
 * changes a rumble by Unload (0x5bfff0) and a new effect and never releases
 * the old one (dinput_pad.c ff_dev_call: an unloaded effect's place is taken
 * again once none is free).
 *
 * The front end: A, B and View also post Enter, Escape and Q there (game.h
 * pad_fe_keys, dinput_pad.c fe_keys): text fields (the profile name) take
 * Enter and Escape only as characters from the window procedure (0x5ccf50 ->
 * FESendKeystroke 0x55dbd0), and the "Are you sure you want to quit?" prompt
 * opens on 'Q'/'q' (0x558b70, if [0x836494] allows it).  The Controls page
 * must keep the device (game.h pad_hold_screen, game_ug2.c). */
#include "cpu.h"
#include <stdio.h>
#include <math.h>

int dipad_guid(uint32_t g);             /* dinput_pad.c: 1 the pad's guidInstance, 2 the wheel's, 0 neither */
int dipad_on(void);                     /* the pad device exists (NFSU2_PAD is not off) */
int dipad_log(void);                    /* NFSU2_PAD_LOG */
int dipad_ff_log(void);                 /* NFSU2_FF_LOG */

/* game.h pad_rescan: the game's own rescan, every 32 frames (0x5cfa10 reads
 * [0x8364e8] every frame) */
void ug2_pad_rescan(int on){
    if (mem_is_committed(0x8364e8u)) MR32(0x8364e8u) = (uint32_t)on;
}

/* The game's input channel that reads this device (0x874c40 + n*0x370, six
 * of them: +0x230 the device's slot in [0x86ffb4], +0x234 its type), or 0. */
static uint32_t dev_channel(uint32_t dev){
    for (uint32_t s = 0; s < 2u; s++){
        if (!dev || MR32(0x86ffb4u + 4u * s) != dev) continue;
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
/* game.h pad_trigger_axes: the triggers of device dev (bit 0 the left, Z at
 * offset 8; bit 1 the right, Rz at 0x14) the game reads as an axis.  Both
 * while no channel reads the device, and while the Joypad page waits for the
 * steering or a pedal (+0x33c 1..4): a trigger should bind there as its axis
 * (past 5 % of its travel), and the page looks at the buttons first
 * (0x5ca720), so a fast pull could bind the button. */
unsigned ug2_pad_trigger_axes(uint32_t dev){
    uint32_t ch = dev_channel(dev);
    if (!ch || MR32(ch + 0x33cu) - 1u < 4u) return 3u;
    return (chan_reads_axis(ch, 0x08u) ? 1u : 0u) | (chan_reads_axis(ch, 0x14u) ? 2u : 0u);
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
    if (dipad_log()) fprintf(stderr, "[pad] default %s layout applied to input channel 0x%08x\n", wheel ? "wheel" : "pad", ch);
}
/* 0x5b81b0(devinfo, tableA, tableB): the game's built-in profile lookup,
 * called from the channel set-up 0x5c11a0 (device attach, and "reset to
 * default" in the controls screen).  cdecl, returns al = 1 when found. */
void sub_005b81b0_orig(CPU *c);
void sub_005b81b0(CPU *c){
    uint32_t ddi = MR32(c->esp + 4u), ta = MR32(c->esp + 8u);
    int ours = dipad_on() && ddi ? dipad_guid(ddi + 4u) : 0;
    if (ours && ta > 0x18u){
        apply_profile(ta - 0x18u, ours == 2);
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
    if (dipad_log()) fprintf(stderr, "[pad] %s bound to a button: its axis is off\n", act == 1u ? "throttle" : "brake");
}
void sub_005b8460_orig(CPU *c);
void sub_005b8460(CPU *c){ bind_takes_axis(c->ecx); sub_005b8460_orig(c); }
void sub_005c1720_orig(CPU *c);
void sub_005c1720(CPU *c){ bind_takes_axis(c->ecx); sub_005c1720_orig(c); }

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
 * for a crash, 1.2 to 5 m/s scraping along a barrier.  game.h pad_jolt:
 * every poll of the pad (dinput_pad.c ff_mix), the level now. */
static float g_jolt;                    /* 0..1, decaying */
static uint32_t g_jolt_car;
#define JOLT_HIST 32
static struct { float v[3], t; } g_jh[JOLT_HIST];   /* velocity at world time t (s), newest at g_jhn - 1 */
static int g_jhn;
static float g_jolt_age, g_jolt_logt;   /* world seconds since this car appeared; since the last speed line */
float ug2_pad_jolt(void){
    uint32_t pl = mem_is_committed(0x8900acu + 3u) ? MR32(0x8900acu) : 0u;
    uint32_t car = pl && mem_is_committed(pl + 7u) ? MR32(pl + 4u) : 0u;
    float dt = MRF32(0x865154u);
    if (MR32(0x8654a4u) != 6u || !car || !mem_is_committed(car + 0x7bu)){ g_jolt_car = 0; g_jolt = 0; return 0.f; }
    float v[3] = { MRF32(car + 0x70u), MRF32(car + 0x74u), MRF32(car + 0x78u) };
    if (!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2])) return g_jolt;
    if (!(dt > 0.f && dt < 0.25f)) return g_jolt;              /* the world stands (pause, a menu over it) */
    g_jolt_age = car == g_jolt_car ? g_jolt_age + dt : 0.f;
    if (dipad_ff_log() && (g_jolt_logt += dt) >= 2.f){
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
            if (lvl > g_jolt + 0.05f && dipad_ff_log()) fprintf(stderr, "[ff] jolt %.2f m/s in %.1f ms: level %.2f\n", (double)dv, (double)span * 1000.0, (double)lvl);
            if (lvl > g_jolt) g_jolt = lvl;
        }
    }
    { int n = g_jhn % JOLT_HIST; g_jh[n].v[0] = v[0]; g_jh[n].v[1] = v[1]; g_jh[n].v[2] = v[2]; g_jh[n].t = now; g_jhn++; }
    g_jolt *= expf(-dt / 0.12f);
    if (g_jolt < 0.01f) g_jolt = 0.f;
    g_jolt_car = car;
    return g_jolt;
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
        if (dipad_log()) fprintf(stderr, "[pad] player input controller at 0x%08x\n", ctx);
    }
    sub_00407750_orig(c);
}
