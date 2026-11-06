/* bf_respawn.c -- Bayview Forever's Respawn at Garage: loading a profile, or
 * the pause menu's Quit to the main menu, starts the next free roam at the
 * player's garage instead of where the car was left.  Shops and races still put
 * the car back where it was.
 *
 * The game (SPEED2.EXE v1.2 NA, as read for this file; carspot.c and
 * savegame.c have the same fields):
 *  - The career (g_Career 0x85AD40) keeps the car's spot for the world's next
 *    load: +0x8744 (byte) set = use it, +0x8750 x, +0x8754 y, +0x8758 z (+0x875C
 *    a fourth float the game neither writes nor reads here), +0x8760 (u16) the
 *    heading; +0x873D (byte) the career has a car of its own.  Free roam's
 *    start (0x54069E..0x5406BF) reads +0x8744, the place and the heading.
 *  - Career_LoadFromBuffer 0x529230 (thiscall career (in, size), ret 8; returns
 *    the next input in eax; its only caller 0x53450F in the profile load
 *    0x5344D0, which garage.c overrides) loads +0x8744 (0x529312), +0x8750
 *    (0x529338) and +0x8760 (0x529359) as the profile saved them.
 *  - Career_OnEnterShop 0x54CAF0 (thiscall career (shop), ret 4; its only caller
 *    0x499497, ecx = g_Career, shop = [0x86344C] when [0x863444], else 0) keeps
 *    the car's spot ([[0x8900AC] +4] +0x60.., +0x40C) at +0x8744/+0x8750/+0x8760
 *    (0x54CBFC..) for the way back out.  shop +0x50 (byte) 0: the player's
 *    garage (the mod's reading; the game also sets career +0x8868 = 1 then).
 *  - The pause menu's Quit (UI_Pause's notify 0x4C2170: its two calls
 *    0x4C21F7 / 0x4C2444 of 0x4A7CF0, thiscall, plain RET, no other caller) goes
 *    to the main menu; it keeps the car's spot first (0x4A7D2F: CALL 0x5139C0).
 *
 * What the mod does (BayviewForever.asi 0x712D78C0, hooks by CALL rewrites):
 *  - 0x53450F -> 0x712D77B0: after the load, the garage spot (when known) goes
 *    into +0x8744 = 1, +0x8750..+0x875C, +0x8760, if the career has a car (+0x873D).
 *  - 0x499497 -> 0x712D75E0: after the game kept the spot of a shop with
 *    +0x50 = 0 (+0x8744 set), that spot is the garage spot (written to
 *    BayviewForever\garage-spot.txt when it changed).
 *  - 0x4C21F7, 0x4C2444 -> 0x712D7510: after the quit, in a career ([0x850078]
 *    = 1) with a car of its own, the garage spot over the one kept.
 *  - The spot: garage-spot.txt's line "spot X Y Z W HEADING" (the last valid one),
 *    else a built-in one: 656.049 -102.1455 15.8103, 0, heading 0x6FA0.  So it is
 *    always "known" (the mod's "not known yet" branches never run).
 * Here: overrides of 0x529230, 0x54CAF0 and 0x4A7CF0 (each the one caller's
 * target).  The file is the profile's: "NFS Underground 2\<NAME>\BF\garage-spot.txt"
 * (the page backs up the profile folder), read at each profile load (the name:
 * garage.c's, set by the cars' load just before the career's), the same format.
 *
 * With the port:
 *  - mprace.c's trip to a LAN race leaves free roam by this same Quit (career
 *    mode 1) and comes back by Return to Explore Mode, "which puts the car back
 *    where it was": while a trip runs (MPR_OUT .. MPR_BACK) the quit is left as
 *    the game has it.  Its LAN race's own Quit has mode 0x200: never touched.
 *  - carspot.c mends a kept spot that is no place (NaN, far out): ours is a
 *    place, and it runs after us, the next frame.
 *  - savegame.c (the pause menu's Save) and co-op's reload (coop.c) set +0x8744
 *    themselves, with no quit and no load: untouched.
 *  - Off by default, as in the mod.
 *
 * Settings ([Features]; the page writes them):
 *   NFSU2_BF_FEATURES_RESPAWNATGARAGE  0/1, default: NFSU2_BF_FEATURES_IMMERSIVETRAVEL
 *                                      (the old name, the mod's own fallback), else 0
 */
#include "cpu.h"
#include "mprace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

long autotest_frame(void);
const char *garage_profile_name(void);           /* garage.c: the profile loaded last */
int fileio_resolve_save(const char *win, char *out, size_t outsz);
int fileio_resolve_save_w(const char *win, char *out, size_t outsz);

#define G_CAREER     0x0085ad40u
#define C_HAS_CAR    0x873du      /* byte: the career has a car of its own */
#define C_KEEP       0x8744u      /* byte: the next world puts the car at the spot */
#define C_SPOT       0x8750u      /* float[4]: x y z (w) */
#define C_HEADING    0x8760u      /* u16 */
#define G_GAMEMODE   0x00850078u  /* 1 career */
#define SHOP_KIND    0x50u        /* shop: 0 the garage */
#define SPOT_FILE    "garage-spot.txt"

static int g_on = -1;
static struct { float p[4]; uint16_t h; int from_file; } G;
static const float BUILTIN[4] = { 656.0490112f, -102.1455002f, 15.8102999f, 0.f };
#define BUILTIN_HEADING 0x6fa0u

static int env_on(const char *name, int def){
    const char *e = getenv(name);
    if (!e || !*e) return def;
    return *e != '0' && strcasecmp(e, "off") && strcasecmp(e, "no") && strcasecmp(e, "false");
}
static void respawn_init(void){
    if (g_on >= 0) return;
    g_on = env_on("NFSU2_BF_FEATURES_RESPAWNATGARAGE", 0);
    memcpy(G.p, BUILTIN, sizeof G.p); G.h = BUILTIN_HEADING;
    fprintf(stderr, "[bf-respawn] Respawn at Garage %s\n", g_on ? "on: profile loads and Quit to Main Menu restart at the garage" : "off");
}

static int spot_path(char *out, size_t n, int w){
    const char *name = garage_profile_name();
    if (!name || !name[0]) return 0;
    char win[160];
    snprintf(win, sizeof win, "NFS Underground 2\\%s\\BF\\%s", name, SPOT_FILE);
    return w ? fileio_resolve_save_w(win, out, n) : fileio_resolve_save(win, out, n);
}
/* the profile's garage-spot.txt (the last good line), else the built-in spot */
static void spot_read(void){
    char path[1100], line[256];
    memcpy(G.p, BUILTIN, sizeof G.p); G.h = BUILTIN_HEADING; G.from_file = 0;
    FILE *f = spot_path(path, sizeof path, 0) ? fopen(path, "r") : NULL;
    if (f){
        while (fgets(line, sizeof line, f)){
            float x, y, z, w; unsigned h;
            if (sscanf(line, "spot %f %f %f %f %u", &x, &y, &z, &w, &h) == 5 && h <= 0xffffu){
                G.p[0] = x; G.p[1] = y; G.p[2] = z; G.p[3] = w; G.h = (uint16_t)h; G.from_file = 1;
            }
        }
        fclose(f);
    }
    fprintf(stderr, "[bf-respawn] garage spot %s: %.2f %.2f %.2f, heading %u\n", G.from_file ? "from " SPOT_FILE : "built in",
            (double)G.p[0], (double)G.p[1], (double)G.p[2], G.h);
}
static void spot_write(void){
    char path[1100];
    if (!spot_path(path, sizeof path, 1)){ fprintf(stderr, "[bf-respawn] no profile: %s not written\n", SPOT_FILE); return; }
    char dir[1100]; snprintf(dir, sizeof dir, "%s", path);
    char *sl = strrchr(dir, '/'); if (sl){ *sl = 0; mkdir(dir, 0755); }
    FILE *f = fopen(path, "w");
    if (!f){ fprintf(stderr, "[bf-respawn] COULD NOT write %s\n", path); return; }
    fputs("; BayviewForever Respawn at Garage: where a profile load starts free roam (the spot the game saves when\n"
          "; you drive into your garage). Delete this file and drive into the garage again to take a new one.\n", f);
    fprintf(f, "spot %.4f %.4f %.4f %.4f %u\n", (double)G.p[0], (double)G.p[1], (double)G.p[2], (double)G.p[3], G.h);
    fclose(f);
}
/* the garage spot as the career's kept one */
static void spot_apply(const char *why){
    float was[2] = { MRF32(G_CAREER + C_SPOT), MRF32(G_CAREER + C_SPOT + 4u) };
    MR8(G_CAREER + C_KEEP) = 1;
    for (uint32_t i = 0; i < 4u; i++) MRF32(G_CAREER + C_SPOT + 4u * i) = G.p[i];
    MR16(G_CAREER + C_HEADING) = G.h;
    fprintf(stderr, "[bf-respawn] f=%ld %s: free roam starts at the garage (the spot was %.0f %.0f)\n", autotest_frame(), why,
            (double)was[0], (double)was[1]);
}
static int trip_runs(void){
    int ph = mprace_phase();
    return ph == MPR_OUT || ph == MPR_LAN || ph == MPR_RACE || ph == MPR_BACK;
}

/* thiscall career (in, size) -> the next input, ret 8: Career_LoadFromBuffer */
void sub_00529230_orig(CPU *c);
void sub_00529230(CPU *c){
    respawn_init();
    uint32_t career = c->ecx;
    sub_00529230_orig(c);                        /* (eax is its result: kept) */
    if (!g_on || career != G_CAREER) return;
    spot_read();
    if (!MR8(G_CAREER + C_HAS_CAR)){ fprintf(stderr, "[bf-respawn] profile loaded: no career car yet, start spot left as saved\n"); return; }
    spot_apply("profile loaded");
}

/* thiscall career (shop), ret 4: Career_OnEnterShop */
void sub_0054caf0_orig(CPU *c);
void uparts_shop_entered(CPU *c);                    /* bf_uparts.c: a unique part's shop, RaceToShop off */
void sub_0054caf0(CPU *c){
    uparts_shop_entered(c);
    respawn_init();
    uint32_t career = c->ecx, shop = MR32(c->esp + 4u);
    sub_0054caf0_orig(c);
    if (!g_on || career != G_CAREER || !shop || !mem_is_committed(shop + SHOP_KIND) || MR8(shop + SHOP_KIND) || !MR8(G_CAREER + C_KEEP)) return;
    float p[4]; uint16_t h = MR16(G_CAREER + C_HEADING);
    for (uint32_t i = 0; i < 4u; i++) p[i] = MRF32(G_CAREER + C_SPOT + 4u * i);
    int same = !memcmp(p, G.p, sizeof p) && h == G.h;
    fprintf(stderr, "[bf-respawn] f=%ld drove into the garage at %.2f %.2f %.2f, heading %u%s\n", autotest_frame(),
            (double)p[0], (double)p[1], (double)p[2], h, same ? "" : ": remembered as the start spot");
    if (same) return;
    memcpy(G.p, p, sizeof p); G.h = h; G.from_file = 1;
    spot_write();
}

/* thiscall pause screen: Quit to the main menu (plain RET) */
void sub_004a7cf0_orig(CPU *c);
void sub_004a7cf0(CPU *c){
    respawn_init();
    sub_004a7cf0_orig(c);
    if (!g_on || MR32(G_GAMEMODE) != 1u) return;
    if (trip_runs()){ fprintf(stderr, "[bf-respawn] f=%ld quit for a multiplayer race: the spot kept where the car is\n", autotest_frame()); return; }
    if (!MR8(G_CAREER + C_HAS_CAR)){ fprintf(stderr, "[bf-respawn] quit to the main menu: start spot left as saved (no career car)\n"); return; }
    spot_apply("quit to the main menu");
}
