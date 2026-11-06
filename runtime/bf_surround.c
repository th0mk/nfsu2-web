/* bf_surround.c -- Bayview Forever's surround sound (unit surround,
 * [Surround.*]).  The game's own sound library (EA's,
 * 0x72xxxx) mixes for 1, 2, 4 or 5.1 speakers into ONE looping DirectSound
 * buffer; the PC game only ever asks for stereo.  The mod asks for 5.1 or
 * quad, places the player's own engine, scales the turbo, balances the
 * speakers and labels the option.  The port's audio layer has to carry the
 * game's 4 or 6 channels to the page (spec_hw.md: com.c, audio_web.c, the
 * worklet); this file is the game side.
 *
 * The game (SPEED2.EXE v1.2 NA, verified):
 *   0x721681 Snd_SetOutputMode(mode) cdecl: 0 mono, 1 stereo, 2 quad, 3 5.1;
 *     0x72163D asks the driver (0x724A8E -> 0x7249BF -> probe 0x725DB5, once:
 *     DirectSoundCreate, the primary buffer, then 0x725A85(n, rate) for n =
 *     1, 2, 4, 6: SetFormat on the primary and CreateSoundBuffer of the mix
 *     buffer, WAVEFORMATEXTENSIBLE for 4 (mask 0x33 FL FR BL BR) and 6 (mask
 *     0x3F FL FR C LFE BL BR), 16 bit, half a second, flags 0x18000; its
 *     success is "supported": bytes [0x8B3FFC + mode]); unsupported -> -5.
 *     Not running ([0x8B4608] 0): the mode is stored [0x818084]; running: it
 *     is pending ([0x8B4028], [0x8B402C] = 1) until the sound system is set
 *     up again (a world load: 0x486C4D .. 0x486C78).
 *     Callers: 0x46B2E5 (AudioSettings_SetAudioMode: stereo), 0x46B2F6
 *     (mono), 0x486C6F (world load).  0x724CD0: mode -> speakers [+0x3F]
 *     (1, 2, 4, 5) and LFE [+0x40] (5.1).
 *   Mixer: [0x8B46E8] byte, the channels (1, 2, 4, 6); [0x8B44DF] the main
 *     speakers, [0x8B44E0] LFE.  The 5.1 channel order inside the library:
 *     0 center, 1 front right, 2 rear right, 3 rear left, 4 front left,
 *     5 LFE (the mod's balance and speaker test tables; quad: 0 FR 45 deg,
 *     1 RR 135, 2 RL 225, 3 FL 315, 0x72591F).
 *   Voices: [0x8B466C] + n*0x90, n < word [0x8B460C] (<= 0x100):
 *     +0 handle (its low byte the index), +0x1C angle (word, 0 ahead,
 *     clockwise, 0x4000 right), +0x23 byte (1: placed by direction), +0x26
 *     group byte (a sound's layers), +0x48 float gain, +0x4E byte LFE only,
 *     +0x5F signed byte volume, +0x6D byte active.
 *   0x724F5D Voice_UpdateGains(n): base = vol(+0x5F) * gain(+0x48) / 127;
 *     for ch < [0x8B46E8]: Snd_SetVoiceChannelGain(n, ch, base * pan[ch])
 *     (CALL at 0x724FAC; 0x72905D cdecl: [0x8B4A00] + n*0x5C, gains +0x1C).
 *     pan from 0x725878 Snd_AngleToGains(angle, out): out[i] =
 *     byte [[0x8B473C + 4i] + (angle >> 8)] / 255 for i < [0x8B44DF]; 5.1:
 *     out[5] = 0 (an LFE-only voice gets 0 everywhere and 1 on ch 5).
 *   0x724093 Snd_SetAzimuth(handle, angle, ?) cdecl: the player engine's
 *     layers (CARSFX_PlayerEngine 0x474310: 1 layer 0x473E60 -> CALL
 *     0x473F5A, handle +0xE0; 2 layers 0x474030 -> 0x47412F +0xE0 and
 *     0x474285 +0xFC).  Its only callers.
 *   Turbo (0x47CB1C..): 0x472770 spool update (thiscall, 1 arg; its sound
 *     +0xBC, volume +0xE4; started without a volume when there was none);
 *     its only callers 0x47CB2C, 0x47CBAF.  Blow-off: CALL 0x45EE70
 *     PlayOneShot (thiscall, 5 args) at 0x47CB9D; sound +0xC0, volume +0xE8;
 *     0x45ED13 plays volume x master, capped.
 *   Options, Audio Mode value (0x49ABE1..): FE text by hash 0x50C900(obj,
 *     hash) CALL at 0x49ABFC, hash 0x55DA8BF8 "Stereo"; FEString_SetText
 *     0x535FE0 (thiscall obj, char*).
 *
 * What the mod does:
 *   Speakers (ini 6): 6 -> 5.1, 4 or 5 -> quad, else stereo.  Every request
 *     for stereo becomes it; 5.1 refused -> quad; refused -> what the game
 *     asked.  The layout set is "running" (the menu label).
 *   MenuLabel (1): "Stereo" in the options reads "5.1 Surround" / "Quad".
 *   OwnEnginePlacement (1), with 4+ channels: the player engine's voices
 *     (marked by Snd_SetAzimuth, and every active voice of their group, for
 *     500 ms) get per-channel gains of their own instead of the angle's:
 *     EngineFront F% from three front directions (front left, center, front
 *     right at -45, 0, +45 degrees, weights (1-c)/2, c, (1-c)/2, c =
 *     EngineCenter %), the rest from the two rear ones (+-135, half each);
 *     power-summed, scaled to the loudness of a sound straight ahead, times
 *     EngineVolume %.  AllOwnSounds (1): any other placed voice (+0x23 1,
 *     not LFE-only) whose angle is within 0x100 of an own engine angle of
 *     the last 250 ms while that engine is NOT within 45 degrees of ahead
 *     (the camera in the car: hood or bumper) gets the same gains without
 *     EngineVolume.
 *   Balance (5.1 only): every channel's gain x its %; SubwooferSend %: the
 *     LFE gets base x send more from every voice that is not LFE-only.
 *   BlowOffVolume / SpoolVolume (150 %, 0..400): the turbo's volume words
 *     x % (0..0xFFFF), in any layout, Speakers = 2 too.
 *   SpeakerTest (0): 5.1, driving: the gear picks one channel and every
 *     voice plays only there (1st FL, 2nd C, 3rd FR, 4th RR, 5th RL, 6th LFE).
 *
 *   NFSU2_BF_FEATURES_SURROUND=0
 *   NFSU2_BF_SURROUND_AUDIO_SPEAKERS=6 (2, 4, 6)   NFSU2_BF_SURROUND_AUDIO_MENULABEL=1
 *   NFSU2_BF_SURROUND_OWNCAR_OWNENGINEPLACEMENT=1  ..._ENGINEFRONT=75 ..._ENGINECENTER=50
 *   ..._OWNCAR_ENGINEVOLUME=80 (0..200)            ..._OWNCAR_ALLOWNSOUNDS=1
 *   NFSU2_BF_SURROUND_SOUNDS_BLOWOFFVOLUME=150 / _SPOOLVOLUME=150 (0..400)
 *   NFSU2_BF_SURROUND_BALANCE_FRONTLEFT/CENTER/FRONTRIGHT/REARLEFT/REARRIGHT=100,
 *     _SUBWOOFER=150 (0..200), _SUBWOOFERSEND=0 (0..100)
 *   NFSU2_BF_SURROUND_TEST_SPEAKERTEST=0
 *
 * THE PORT, NOW: the game's 5.1 (and quad) mode is EXPERIMENTAL here and off unless
 * NFSU2_BF_SURROUND_EXPERIMENTAL=1: in it the browser tab crashed at the first
 * world load.  Found with a MEMCHECK build: the sound library's mixer step
 * 0x7292BC clears each output channel's buffer ([0x8B48F0 + 4 ch], allocated by
 * 0x729150 from the library's pool through 0x724792), and in 5.1 those are 0:
 * the library was started (0x72461F, from 0x72183A) with a pool of 0 --
 * [0x8B4050], which 0x72192E sets (from 0x47AC23, the game's 400 KB pool) and
 * the shutdown 0x72191F clears.  So in 5.1 the port runs the library's
 * shutdown and start again (the world load's re-apply, 0x486C4B..0x486C77)
 * without the game giving it its memory again; with stereo it does not.  Not
 * traced further yet.  Until then only the turbo volumes act (any speakers).
 */
#include "cpu.h"
#include "gamepad.h"             /* gamepad_env_flag */
#include "widescreen.h"          /* HOOK_SITE */
#include "splitscreen_mem.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define G_RUNNING   0x008b4608u   /* byte: the sound system runs */
#define G_NCH       0x008b46e8u   /* byte: mixer channels */
#define G_NMAIN     0x008b44dfu   /* byte: main speakers */
#define G_MODE3     0x008b46ecu   /* 3: 5.1 */
#define G_PANTAB    0x008b473cu   /* byte table pointers by channel */
#define G_INV255    0x00784548u
#define G_VOICES    0x008b466cu
#define G_NVOICES   0x008b460cu
#define FN_SETTEXT  0x00535fe0u
#define H_STEREO    0x55da8bf8u

void sub_00721681_orig(CPU *c);
void sub_00472770_orig(CPU *c);
void sub_00724093_orig(CPU *c);
uint32_t com_scratch(uint32_t n);

/* ---- settings ---------------------------------------------------------------- */
static struct {
    int on, mode, label, place, front, center, all, test;
    float engvol, blowoff, spool, bal[6], send; int bal_on;
} S;
static int envi(const char *n, int d, int lo, int hi){
    const char *e = getenv(n); int v = e && *e ? atoi(e) : d;
    return v < lo ? lo : v > hi ? hi : v;
}
static void cfg(void){
    static int done;
    if (done) return;
    done = 1;
    S.on = gamepad_env_flag("NFSU2_BF_FEATURES_SURROUND", 0);
    int spk = envi("NFSU2_BF_SURROUND_AUDIO_SPEAKERS", 6, 0, 8);
    S.mode = spk > 5 ? 3 : spk > 3 ? 2 : 1;
    S.label = gamepad_env_flag("NFSU2_BF_SURROUND_AUDIO_MENULABEL", 1);
    S.place = gamepad_env_flag("NFSU2_BF_SURROUND_OWNCAR_OWNENGINEPLACEMENT", 1);
    S.front = envi("NFSU2_BF_SURROUND_OWNCAR_ENGINEFRONT", 75, 0, 100);
    S.center = envi("NFSU2_BF_SURROUND_OWNCAR_ENGINECENTER", 50, 0, 100);
    S.engvol = envi("NFSU2_BF_SURROUND_OWNCAR_ENGINEVOLUME", 80, 0, 200) / 100.f;
    S.all = gamepad_env_flag("NFSU2_BF_SURROUND_OWNCAR_ALLOWNSOUNDS", 1);
    S.blowoff = envi("NFSU2_BF_SURROUND_SOUNDS_BLOWOFFVOLUME", 150, 0, 400);
    S.spool = envi("NFSU2_BF_SURROUND_SOUNDS_SPOOLVOLUME", 150, 0, 400);
    /* the library's 5.1 order: 0 C, 1 FR, 2 RR, 3 RL, 4 FL, 5 LFE */
    static const char *const key[6] = {"CENTER", "FRONTRIGHT", "REARRIGHT", "REARLEFT", "FRONTLEFT", "SUBWOOFER"};
    for (int i = 0; i < 6; i++){
        char n[64]; snprintf(n, sizeof n, "NFSU2_BF_SURROUND_BALANCE_%s", key[i]);
        int v = envi(n, i == 5 ? 150 : 100, 0, 200);
        S.bal[i] = v / 100.f;
        if (v != 100) S.bal_on = 1;
    }
    S.send = envi("NFSU2_BF_SURROUND_BALANCE_SUBWOOFERSEND", 0, 0, 100) / 100.f;
    if (S.send > 0.f) S.bal_on = 1;
    S.test = gamepad_env_flag("NFSU2_BF_SURROUND_TEST_SPEAKERTEST", 0);
    if (S.on) fprintf(stderr, "[surround] on: %s, own engine %s (%d %% front, %d %% center, %.0f %%), turbo blow-off %.0f %%, "
                      "spool %.0f %%, balance %s\n", S.mode == 3 ? "5.1" : S.mode == 2 ? "quad" : "stereo",
                      S.place ? "placed" : "the game's", S.front, S.center, S.engvol * 100, S.blowoff, S.spool,
                      S.bal_on ? "on" : "off");
}
static uint32_t now_ms(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    uint32_t m = (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u);
    return m ? m : 1u;
}

/* ---- the layout: Snd_SetOutputMode ------------------------------------------------ */
static int g_running_mode = 1;   /* what was set (the menu label) */
__attribute__((weak)) int audio_max_channels(void);   /* audio_web.c: 6 with the surround output */
void sub_00721681(CPU *c){
    cfg();
    uint32_t asked = MR32(c->esp + 4u);
    /* (no 6-channel output: stereo; audio_web.c opens one only with NFSU2_BF_SURROUND_EXPERIMENTAL, see the top) */
    if (!S.on || asked != 1u || S.mode == 1 || !audio_max_channels || audio_max_channels() < 6){
        sub_00721681_orig(c);
        if ((int32_t)c->eax >= 0) g_running_mode = (int)asked;
        return;
    }
    uint32_t want = (uint32_t)S.mode;
    MR32(c->esp + 4u) = want; sub_00721681_orig(c);
    if ((int32_t)c->eax < 0 && want == 3u){
        fprintf(stderr, "[surround] 5.1 refused (%d), trying quad\n", (int)c->eax);
        want = 2u; MR32(c->esp + 4u) = want; sub_00721681_orig(c);
    }
    if ((int32_t)c->eax < 0){
        fprintf(stderr, "[surround] %s refused (%d): the game's stereo\n", want == 3u ? "5.1" : "quad", (int)c->eax);
        want = asked; MR32(c->esp + 4u) = want; sub_00721681_orig(c);
    }
    MR32(c->esp + 4u) = asked;                    /* cdecl: the caller's argument as it was */
    if ((int32_t)c->eax >= 0) g_running_mode = (int)want;
    fprintf(stderr, "[surround] output mode: stereo asked -> %u, result %d (%s)\n", want, (int)c->eax,
            MR8(G_RUNNING) ? "running: applies at the next world load" : "not running yet");
}

/* ---- the turbo ---------------------------------------------------------------------- */
static void scale_word(uint32_t a, float pct){
    int64_t v = (int64_t)(int32_t)MR32(a) * (int64_t)pct / 100;
    MR32(a) = (uint32_t)(v < 0 ? 0 : v > 0xffff ? 0xffff : v);
}
void sub_00472770(CPU *c){                       /* spool update (thiscall, ret 4) */
    cfg();
    uint32_t self = c->ecx, had = MR32(self + 0xbcu);
    sub_00472770_orig(c);
    if (S.on && had && S.spool != 100.f) scale_word(self + 0xe4u, S.spool);
}
static uint32_t g_blowoff_obj, g_blowoff_had;

/* ---- the player's engine: Snd_SetAzimuth ------------------------------------------------- */
static uint32_t g_mark[256];                     /* own engine voice: when marked (ms) */
static struct { uint32_t t; uint16_t angle; } g_eng_angle[4];
static unsigned g_eng_n;
static uint8_t g_other[256];                     /* another own-car voice (decided on channel 0) */
static uint32_t voice(uint32_t i){ return MR32(G_VOICES) + i * 0x90u; }
void sub_00724093(CPU *c){
    cfg();
    int32_t handle = (int32_t)MR32(c->esp + 4u);
    unsigned n = MR16(G_NVOICES);
    int use = S.on && S.place && handle >= 0 && MR32(G_VOICES) && n <= 0x100u && (unsigned)(handle & 0xff) < n;
    unsigned idx = (unsigned)handle & 0xffu;
    if (use && MR32(voice(idx)) == (uint32_t)handle && MR8(voice(idx) + 0x6du)){
        uint32_t t = now_ms();
        g_mark[idx] = t;
        uint8_t grp = MR8(voice(idx) + 0x26u);
        if (grp) for (unsigned k = 0; k < n; k++)
            if (k != idx && MR8(voice(k) + 0x26u) == grp && MR8(voice(k) + 0x6du) == 1 && (int32_t)MR32(voice(k)) >= 0) g_mark[k] = t;
    }
    sub_00724093_orig(c);
    if (use && S.all && MR32(voice(idx)) == (uint32_t)handle){
        unsigned k = (++g_eng_n) & 3u;
        g_eng_angle[k].angle = MR16(voice(idx) + 0x1cu);
        g_eng_angle[k].t = now_ms();
    }
}

/* ---- the own-car gains: as the mod, from the game's own pan tables ---------------------- */
static float g_tab_other[8], g_tab_engine[8];
static int g_tab_nch = -1;
static void angle_gains(uint16_t angle, float out[8]){
    memset(out, 0, 8 * sizeof(float));
    unsigned nm = MR8(G_NMAIN);
    for (unsigned i = 0; i < nm && i < 8; i++){
        uint32_t t = MR32(G_PANTAB + 4u * i);
        out[i] = t ? (float)MR8(t + (angle >> 8)) * MRF32(G_INV255) : 0.f;
    }
    if (MR32(G_MODE3) == 3u) out[5] = 0.f;
}
static void own_tables(int nch){
    float g[8], e[8] = {0}, f = S.front / 100.f, cc = S.center / 100.f;
    static const uint16_t front[3] = {0xe000, 0x0000, 0x2000}, rear[2] = {0xa000, 0x6000};
    float w[3] = {(1.f - cc) * 0.5f, cc, (1.f - cc) * 0.5f};
    for (int k = 0; k < 3; k++){ angle_gains(front[k], g); for (int i = 0; i < 8; i++) e[i] += f * w[k] * g[i] * g[i]; }
    for (int k = 0; k < 2; k++){ angle_gains(rear[k], g); for (int i = 0; i < 8; i++) e[i] += (1.f - f) * 0.5f * g[i] * g[i]; }
    angle_gains(0, g);
    float se = 0.f, s0 = 0.f;
    for (int i = 0; i < 8; i++){ se += e[i]; s0 += g[i] * g[i]; }
    float norm = se > 0.f ? s0 / se : 0.f;       /* GUESS for se == 0: the mod's branch keeps another value */
    for (int i = 0; i < 8; i++){
        g_tab_other[i] = i < nch ? sqrtf(norm * e[i]) : 0.f;
        g_tab_engine[i] = g_tab_other[i] * S.engvol;
    }
    g_tab_nch = nch;
}
static int g_test_ch = -1;                       /* the speaker test's channel (-1: the normal mix) */

/* ---- hooks ------------------------------------------------------------------------------ */
int bf_surround_hook(CPU *c, uint32_t ip){
    switch (ip){
    case HOOK_SITE(0x0047cb9d):                  /* CALL PlayOneShot (the blow-off), esi = ecx = the turbo */
        g_blowoff_obj = c->esi; g_blowoff_had = MR32(c->esi + 0xc0u);
        return 1;
    case HOOK_SITE(0x0047cba2):                  /* after it */
        cfg();
        if (S.on && c->esi == g_blowoff_obj && !g_blowoff_had && MR32(c->esi + 0xc0u) && S.blowoff != 100.f)
            scale_word(c->esi + 0xe8u, S.blowoff);
        return 1;
    case HOOK_SITE(0x00724fac): {                /* CALL Snd_SetVoiceChannelGain(n, ch, gain) */
        cfg();
        if (!S.on) return 1;
        uint32_t n = MR32(c->esp), ch = MR32(c->esp + 4u);
        if (n > 0xffu || ch > 7u) return 1;
        int nch = MR8(G_NCH);
        uint32_t v = voice(n);
        float base = (float)(int8_t)MR8(v + 0x5fu) * MRF32(v + 0x48u) * (1.f / 127.f), gain = MRF32(c->esp + 8u);
        if (S.test && nch == 6 && g_test_ch >= 0){ MRF32(c->esp + 8u) = (int)ch == g_test_ch ? base : 0.f; return 1; }
        int own = 0;                             /* 1 the engine, 2 another own-car sound */
        if (S.place && nch > 3){
            uint32_t m = g_mark[n];
            if (m && now_ms() - m <= 499u) own = 1;
            else {
                if (ch == 0){
                    g_other[n] = 0;
                    if (S.all && MR8(v + 0x23u) == 1 && !MR8(v + 0x4eu)){
                        uint16_t a = MR16(v + 0x1cu); uint32_t t = now_ms();
                        for (int k = 0; k < 4; k++){
                            if (!g_eng_angle[k].t || t - g_eng_angle[k].t > 250u) continue;
                            int16_t ea = (int16_t)g_eng_angle[k].angle;
                            if (ea >= -0x1fff && ea <= 0x1fff) continue;      /* the engine ahead: a chase view */
                            int16_t d = (int16_t)(a - g_eng_angle[k].angle);
                            if (d >= -0x100 && d <= 0x100){ g_other[n] = 1; break; }
                        }
                    }
                }
                if (g_other[n]) own = 2;
            }
            if (own){
                if (g_tab_nch != nch) own_tables(nch);
                gain = base * (own == 1 ? g_tab_engine : g_tab_other)[ch];
            }
        }
        if (S.bal_on && nch == 6){
            if (ch == 5 && !MR8(v + 0x4eu) && S.send > 0.f) gain += S.send * base;
            gain *= S.bal[ch < 6 ? ch : 0];
        }
        MRF32(c->esp + 8u) = gain;
        return 1;
    }
    case HOOK_SITE(0x0049ac01): {                /* after FE text by hash: [esp] obj, [esp+4] hash */
        cfg();
        uint32_t obj = MR32(c->esp), hash = MR32(c->esp + 4u);
        if (!S.on || !S.label || hash != H_STEREO || (g_running_mode != 2 && g_running_mode != 3)) return 1;
        if (!obj || !mem_is_committed(obj + 0x6fu) || MR32(obj + 0x18u) != 2u) return 1;
        static uint32_t txt[2];
        int k = g_running_mode == 3 ? 0 : 1;
        if (!txt[k]){
            const char *s = k ? "Quad" : "5.1 Surround";   /* language.c could give it per language */
            txt[k] = com_scratch(16);
            for (unsigned i = 0; i <= strlen(s); i++) MR8(txt[k] + i) = (uint8_t)s[i];
        }
        CPU save = *c;
        c->esp = (c->esp - 256u) & ~15u;
        PUSH32(txt[k]); c->ecx = obj; PUSH32(FN_SETTEXT);
        dispatch_indirect(c, FN_SETTEXT);
        *c = save;
        MR32(obj + 0x1cu) |= 2u;
        return 1;
    }
    }
    return 0;
}

/* per frame (roam.c's 0x581475 list): the speaker test follows player 1's gear */
void bf_surround_frame(CPU *c){
    (void)c;
    cfg();
    if (!S.on || !S.test){ g_test_ch = -1; return; }
    static const int by_gear[6] = {4, 0, 1, 2, 3, 5};   /* 1st FL, 2nd C, 3rd FR, 4th RR, 5th RL, 6th LFE */
    uint32_t p = MR32(SPLIT_PLAYERS_INDEX), car = p && mem_is_committed(p + 7u) ? MR32(p + 4u) : 0u;
    int gear = car && mem_is_committed(car + 0x437u) ? (int)MR32(car + 0x434u) : -1;
    int ch = gear >= 2 && gear <= 7 ? by_gear[gear - 2] : -1;
    if (ch != g_test_ch){
        static const char *const nm[6] = {"center", "front right", "rear right", "rear left", "front left", "subwoofer"};
        fprintf(stderr, "[surround] speaker test: gear %d -> %s\n", gear, ch < 0 ? "normal mix" : nm[ch]);
        g_test_ch = ch;
    }
}
