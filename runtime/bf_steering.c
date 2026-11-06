/* bf_steering.c -- Bayview Forever's wheel steering options (unit wheel,
 * [Wheel.Steering]; DRAFT for the port).  The [Wheel.ForceFeedback] half of
 * the mod is not ported: a browser cannot drive a wheel's motor.
 *
 * How the game steers with a wheel (SPEED2.EXE v1.2 NA):
 *   0x41E2C0 (RealDriver update) at 0x41E372 calls the steering object's
 *   update 0x41DFC0 (thiscall ecx = driver+0x220; args: steer, dt, port;
 *   ret 0xC).  steer is driver+0x2C8: the action handler 0x407750 makes it of
 *   the channel's BYTE (0..255): (b - [0x784540]) * [0x78453C], so 255 steps
 *   with a wider middle step ("FullResolution").  0x41DFC0: 0x5C1000(port)
 *   (the port's device is a driving device: [0x86E5BC + slot]) -> 0x4061C0,
 *   else 0x416BB0 (keyboard / pad: turns the wheels at a limited rate).
 *   0x4061C0 (the wheel path, the one runtime/dinput_pad.c's driving device
 *   takes) maps steer through two tables, each {count, min, max, 1/step, ptr}
 *   and looked up by 0x5764D0:
 *     0x7E9E88 -> 0x7E92D8: 21 floats over -1..1, the response curve
 *       game: -1 -.8 -.615 -.483 -.388 -.288 -.22 -.161 -.111 -.057 0 (and
 *       mirrored): half the wheel's way gives 0.288 of the lock
 *     0x7E9EEC -> 0x7E91A4: 10 floats over 0..160 (car+0x42C speed, m/s;
 *       a step 17.8 m/s = 64 km/h), the front wheels' lock in degrees:
 *       45 15 11 8 7 7 7 7 7 7
 *   Those two tables are read by 0x4061C0 only (their descriptors' only
 *   references: 0x406203, 0x40623B), so changing them changes a wheel's
 *   steering and nothing else.
 *
 * What the mod does:
 *   Linearity L (0..100, ini 100): curve[i] = game[i] + (x_i - game[i]) * L/100,
 *     x_i = i/10 - 1; then ends -1, +1 and the middle 0 exact.  0: the game's.
 *   SpeedSensitivity S (0..100, ini 80): lock[i] = 45 - (45 - game[i]) * S/100.
 *     100: the game's (not written).
 *   FullResolution (ini 1): replaces the CALL at 0x41E372: for a driving
 *     device on port 0 or 1, the steering is read again from the device's
 *     DIJOYSTATE2 ([0x870760] + port*0x110 + the channel's axis offset
 *     +0x344, sign +0x354 (signed byte), the channel 0x874C40 + n*0x370 with
 *     +0x230 == port, +0x234 != 0x13, +0x364 != 0): v = -raw*sign/10000,
 *     clamped to -1..1, and used instead of the byte's value when the two are
 *     within 0.06 (else the game's).
 *   WheelRotation W / SteeringLock K (40..1080 degrees, ini 450/450; K <= W):
 *     the steering times W/K, clamped to -1..1, times the steering object's
 *     +8 (its range), passed on.  So full lock at K degrees lock to lock.
 *
 * In the port: the page already scales the wheel (web/src/input/wheel.ts
 * WheelMap.lock: DEFAULT_LOCK_DEGREES 270 of a known model's rotation, or the
 * player's "Set up the wheel"), which is W/K.  So W/K is NOT applied here
 * (spec_hw.md: map SteeringLock to the page's lock).  Linearity,
 * SpeedSensitivity and FullResolution are new to the port and done here.
 *
 *   NFSU2_BF_FEATURES_WHEEL=1              the whole [Wheel.Steering] part
 *   NFSU2_BF_WHEEL_STEERING_FULLRESOLUTION=1
 *   NFSU2_BF_WHEEL_STEERING_LINEARITY=100      0..100 (0 = the game's curve)
 *   NFSU2_BF_WHEEL_STEERING_SPEEDSENSITIVITY=80 0..100 (100 = the game's)   */
#include "cpu.h"
#include "gamepad.h"
#include "widescreen.h"          /* HOOK_SITE */
#include <stdio.h>
#include <stdlib.h>

#define T_CURVE     0x007e92d8u   /* 21 floats, -1..1 */
#define T_LOCK      0x007e91a4u   /* 10 floats, 0..160 m/s, degrees */
#define G_DEVSTATES 0x00870760u   /* -> DIJOYSTATE2 records, 0x110 each by port */
#define G_CHANNELS  0x00874c40u   /* 6 input channels, 0x370 each */
#define G_CHAN_END  0x008760e0u

static const float k_curve[21] = { -1.f, -.8f, -.615f, -.483f, -.388f, -.288f, -.22f, -.161f, -.111f, -.057f, 0.f,
                                    .057f, .111f, .161f, .22f, .288f, .388f, .483f, .615f, .8f, 1.f };
static const float k_lock[10] = { 45.f, 15.f, 11.f, 8.f, 7.f, 7.f, 7.f, 7.f, 7.f, 7.f };

static int g_on = -1, g_fullres;
static float g_lin, g_sens;
static float envf01(const char *n, float d){
    const char *e = getenv(n);
    float v = e && *e ? (float)atof(e) : d;
    return v < 0.f ? 0.f : v > 100.f ? 1.f : v / 100.f;
}
static void cfg(void){
    if (g_on >= 0) return;
    g_on = gamepad_env_flag("NFSU2_BF_FEATURES_WHEEL", 0);
    g_fullres = gamepad_env_flag("NFSU2_BF_WHEEL_STEERING_FULLRESOLUTION", 1);
    g_lin = envf01("NFSU2_BF_WHEEL_STEERING_LINEARITY", 100.f);
    g_sens = envf01("NFSU2_BF_WHEEL_STEERING_SPEEDSENSITIVITY", 80.f);
}

/* the two tables: written once, from the game's values (checked first, as the mod does) */
static void tables(void){
    static int done;
    if (done || !g_on) return;
    done = 1;
    for (int i = 0; i < 21; i++) if (MRF32(T_CURVE + 4u*i) - k_curve[i] > 5e-4f || k_curve[i] - MRF32(T_CURVE + 4u*i) > 5e-4f){
        fprintf(stderr, "[wheel] the response curve is not the game's: left as it is\n"); goto lock; }
    if (g_lin > 0.f){
        for (int i = 0; i < 21; i++){
            float x = (float)i * 0.1f - 1.f;
            MRF32(T_CURVE + 4u*i) = k_curve[i] + (x - k_curve[i]) * g_lin;
        }
        MRF32(T_CURVE) = -1.f; MRF32(T_CURVE + 40u) = 0.f; MRF32(T_CURVE + 80u) = 1.f;
        fprintf(stderr, "[wheel] response curve: 10%% -> %.3f, 50%% -> %.3f (game 0.057, 0.288)\n",
                MRF32(T_CURVE + 44u), MRF32(T_CURVE + 60u));
    }
lock:
    for (int i = 0; i < 10; i++) if (MRF32(T_LOCK + 4u*i) - k_lock[i] > 5e-4f || k_lock[i] - MRF32(T_LOCK + 4u*i) > 5e-4f){
        fprintf(stderr, "[wheel] the lock table is not the game's: left as it is\n"); return; }
    if (g_sens < 1.f){
        for (int i = 0; i < 10; i++) MRF32(T_LOCK + 4u*i) = 45.f - (45.f - k_lock[i]) * g_sens;
        fprintf(stderr, "[wheel] lock by speed (0, 64, 128, 192, 256 km/h): %.1f %.1f %.1f %.1f %.1f deg (game 45 15 11 8 7)\n",
                MRF32(T_LOCK), MRF32(T_LOCK + 4u), MRF32(T_LOCK + 8u), MRF32(T_LOCK + 12u), MRF32(T_LOCK + 16u));
    }
}

/* FullResolution: the device's own axis for the byte's value */
static int full_res(uint32_t port, float *steer){
    uint32_t base = MR32(G_DEVSTATES);
    if (port > 1u || !base) return 0;
    for (uint32_t ch = G_CHANNELS; ch < G_CHAN_END; ch += 0x370u){
        if (MR32(ch + 0x230u) != port || MR32(ch + 0x234u) == 0x13u || !MR8(ch + 0x364u)) continue;
        uint32_t ofs = MR32(ch + 0x344u);
        int8_t sign = (int8_t)MR8(ch + 0x354u);
        if (ofs > 0x10cu || (ofs & 3u) || !sign) continue;
        uint32_t a = base + port * 0x110u + ofs;
        if (!mem_is_committed(a + 3u)) return 0;
        float v = -(float)(int32_t)MR32(a) * (float)sign / 10000.f;
        if (v > 1.f) v = 1.f; if (v < -1.f) v = -1.f;
        float d = v - *steer;
        if (d < 0.f) d = -d;
        if (d <= 0.06f){ *steer = v; return 1; }   /* else another binding: the game's value */
        return 0;
    }
    return 0;
}

int bf_steering_hook(CPU *c, uint32_t ip){
    if (ip != HOOK_SITE(0x0041e372)) return 0;   /* CALL 0x41DFC0: [esp] steer, [esp+4] dt, [esp+8] port */
    cfg();
    if (!g_on) return 1;
    tables();
    if (!g_fullres) return 1;
    uint32_t port = MR32(c->esp + 8u);
    /* 0x5C1000(port), inline: a device in the slot ([0x86FFB4 + 4 port]) and
     * it is a driving device ([0x86E5BC + port], set by 0x5CD1F0) */
    if (port > 1u || !MR32(0x0086ffb4u + 4u * port) || !MR8(0x0086e5bcu + port)) return 1;
    /* the mod's precondition: the steering object's (ecx, driver+0x220) range +8 within 0.05 .. 2 */
    float range = MRF32(c->ecx + 8u), s = MRF32(c->esp);
    if (!(range >= 0.05f && range <= 2.f)) return 1;
    if (full_res(port, &s)) MRF32(c->esp) = s;
    return 1;
}
