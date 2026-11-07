/* carpaint.c -- a car's exhaust tips, brake calipers and rims keep their own paint (or none), whatever paint other
 * cars with the same parts have.
 *
 * A part's paint is a light material.  A model's solid has a table of them (solid +0x3C, +0x1A entries of 8 bytes: a
 * name hash and the material), filled when the solid loads (0x493D31: the material of that name, 0x48F6A0's lookup on
 * the list at 0x8358C0, else the one named DEFAULT, [0x7F399C]).  Drawing a car (0x620AE0) puts the car's own paint in
 * it just before it draws the part (0x48D860, eModel::ReplaceLightMaterial, which writes the solid's table):
 * EXHAUST_TIP with the render info's exhaust paint (+0x394, 0x624C21), CALIPER and 0xA3186E2B with its brake paint
 * (+0x398, 0x62E057), MAGSILVER, MAGCHROME and MAGGUNMETAL with its rim paint (+0x390, 0x6277B1).  The render info
 * (0x638190) leaves those at 0 for a car without that paint (0x638DD8..0x63908B: a stock part, level 0, or no paint
 * bought), and the drawing then writes nothing: the part is drawn with whatever the solid's table holds.  The other
 * drawing, 0x6162E0 (0x6316D0 calls it instead of 0x620AE0), writes neither the tips' nor the calipers' paint.  In
 * the game only the player's car has the shops' parts out in the world (the models of the shared packs, CARS\EXHAUST,
 * CARS\BRAKES, CARS\WHEELS\GEOMETRY_<brand>, are unloaded there), so the table held the player's paint or what the
 * solid loaded with.  With other players' cars (free roam, co-op) many cars draw the same solid, and an unpainted
 * part took the paint of the last car drawn before it with a painted one of that style: a grey tip turned blue,
 * green or yellow as painted cars drove by, and stayed so into the garage (players, 2026-11-04..07); the same for
 * calipers and rims.
 * So before a car is drawn (showoff.c's override of 0x620AE0, photo.c's of 0x6162E0) its tips' and calipers' models
 * get the material their solid loaded with, then the car's own paint if it has one, as 0x620AE0 gives them (the tips
 * ride slot 0x10, the calipers 0x1B; 0x1C, the rear brakes, as loaded), and its rims, if it has no rim paint, the
 * ones they loaded with (with rim paint the game's own writes stay: they are not one plain write per model, one of
 * them goes to a model from a stack slot that the loop has not filled yet, 0x6277E5).  What each car shows is then
 * what it would show as the only car with those parts, as in the game.
 * NFSU2_TEST_PAINT_GAMEWAY=1 (script runs only): the game's way, for an A/B.
 * Test aid ("roam mp paint", roam_game.c): carpaint_watch counts each such part's draw (overrides.c sub_005c5930)
 * whose solid holds another car's paint: an unpainted part's not its default, a painted tip's not its paint. */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CRI_MODELS     0x17a8u        /* render info: each ride slot's models, 0x20 bytes a slot (2 x 4 LODs) */
#define CRI_RIM_PAINT  0x390u
#define CRI_TIP_PAINT  0x394u
#define CRI_BRK_PAINT  0x398u
#define CRI_CAR        0u             /* render info: its car (car +0x554 the render info) */
#define MODEL_SOLID    0xcu           /* eModel: its solid */
#define SOLID_NLM      0x1au          /* solid: how many light materials (signed byte) */
#define SOLID_LM       0x3cu          /* solid: the light materials, {name hash, material} */
#define G_LM_LIST      0x008358c0u    /* the light materials' list (a material's link at its +4), 0x48F6A0 */
#define G_LM_DEFAULT   0x007f399cu    /* the material named DEFAULT: what a name not on the list gets */
#define LM_HASH        0xcu
#define G_PLAYER1      SPLIT_PLAYERS_INDEX   /* Player*: +4 its car */

enum { H_TIP, H_CALIPER, H_CALIPER2, H_MAGSILVER, H_MAGCHROME, H_MAGGUNMETAL, NH };
static const uint32_t g_hash[NH] = {
    0x3b3a1f8du,                      /* EXHAUST_TIP */
    0xd6640dffu,                      /* CALIPER */
    0xa3186e2bu,                      /* the calipers' other (0x62E066) */
    0x22719fa9u, 0xfd102a92u, 0x12049251u,   /* MAGSILVER, MAGCHROME, MAGGUNMETAL */
};

static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n); }
static uint32_t rd32(uint32_t a){ return ok(a, 3) ? MR32(a) : 0u; }

/* the material each name's solids loaded with (0x48F6A0), kept while the list's ends and DEFAULT are the same */
static uint32_t g_def[NH], g_def_key[3];
static void defaults(void){
    uint32_t head = rd32(G_LM_LIST), tail = rd32(G_LM_LIST + 4u), dflt = rd32(G_LM_DEFAULT);
    if (g_def_key[0] == head && g_def_key[1] == tail && g_def_key[2] == dflt && head) return;
    g_def_key[0] = head; g_def_key[1] = tail; g_def_key[2] = dflt;
    for (int i = 0; i < NH; i++) g_def[i] = 0;
    const uint32_t end = G_LM_LIST - 4u;
    uint32_t m = head ? head - 4u : 0u;
    for (int n = 0; m && m != end && ok(m, LM_HASH + 4u) && n < 65536; n++){
        uint32_t h = MR32(m + LM_HASH);
        for (int i = 0; i < NH; i++) if (h == g_hash[i] && !g_def[i]) g_def[i] = m;
        uint32_t nx = MR32(m + 4u);
        m = nx ? nx - 4u : 0u;
    }
    for (int i = 0; i < NH; i++) if (!g_def[i]) g_def[i] = dflt;
}

/* each model of ride slot sl: its solid's entries of names [h0, h1) get material mat (as 0x48D860 writes them) */
static void put(uint32_t ri, uint32_t sl, int h0, int h1, const uint32_t *mat){
    for (uint32_t j = 0; j < 8u; j++){
        uint32_t mdl = MR32(ri + CRI_MODELS + 0x20u * sl + 4u * j), so = mdl ? rd32(mdl + MODEL_SOLID) : 0u;
        if (!ok(so, SOLID_LM + 4u)) continue;
        int n = (int8_t)MR8(so + SOLID_NLM);
        uint32_t t = MR32(so + SOLID_LM);
        if (n <= 0 || !ok(t, 8u * (uint32_t)n)) continue;
        for (int e = 0; e < n; e++){
            uint32_t h = MR32(t + 8u * (uint32_t)e);
            for (int i = h0; i < h1; i++) if (h == g_hash[i] && mat[i]) MR32(t + 8u * (uint32_t)e + 4u) = mat[i];
        }
    }
}

static int g_gameway = -1;
uint32_t g_carpaint_ri;               /* the render info being drawn (the test aid's) */
void carpaint_draw(uint32_t ri){
    if (g_gameway < 0){ const char *s = getenv("NFSU2_INPUT_SCRIPT"), *e = getenv("NFSU2_TEST_PAINT_GAMEWAY");
                        g_gameway = s && *s && e && *e == '1'; }
    g_carpaint_ri = ri;
    if (g_gameway || !ok(ri, CRI_MODELS + 0x20u * 0x3fu)) return;
    defaults();
    uint32_t tip = MR32(ri + CRI_TIP_PAINT), brk = MR32(ri + CRI_BRK_PAINT), rim = MR32(ri + CRI_RIM_PAINT), own[NH];
    for (int i = 0; i < NH; i++) own[i] = g_def[i];
    put(ri, 0x10u, H_TIP, H_TIP + 1, own);
    put(ri, 0x1bu, H_CALIPER, H_CALIPER2 + 1, own);
    put(ri, 0x1cu, H_CALIPER, H_CALIPER2 + 1, own);
    if (!rim){ put(ri, 0x1du, H_MAGSILVER, NH, own); put(ri, 0x1eu, H_MAGSILVER, NH, own); }
    for (int i = 0; i < NH; i++) own[i] = 0;
    own[H_TIP] = tip; own[H_CALIPER] = own[H_CALIPER2] = brk;
    if (tip) put(ri, 0x10u, H_TIP, H_TIP + 1, own);
    if (brk) put(ri, 0x1bu, H_CALIPER, H_CALIPER2 + 1, own);
}
void carpaint_drawn(void){ g_carpaint_ri = 0; }

/* ---- the test aid: each part's draw checked (overrides.c sub_005c5930 calls carpaint_model while carpaint_watch) ---- */
extern int g_carpaint_watch;          /* overrides.c */
static struct { uint64_t n, bad, mine, mine_bad, kn[3], kbad[3]; uint32_t last_h, last_has, last_want; } W;
void carpaint_model(uint32_t mdl){
    uint32_t ri = g_carpaint_ri;
    if (!ri || !mdl || !ok(ri, CRI_MODELS + 0x20u * 0x3fu)) return;
    static const uint8_t slots[5] = { 0x10u, 0x1bu, 0x1cu, 0x1du, 0x1eu };
    int s = -1;
    for (int k = 0; k < 5 && s < 0; k++)
        for (uint32_t j = 0; j < 8u; j++) if (MR32(ri + CRI_MODELS + 0x20u * slots[k] + 4u * j) == mdl){ s = k; break; }
    if (s < 0) return;
    uint32_t paint = s == 0 ? MR32(ri + CRI_TIP_PAINT) : s <= 2 ? MR32(ri + CRI_BRK_PAINT) : MR32(ri + CRI_RIM_PAINT);
    if (paint && s != 0) return;      /* painted calipers and rims: not checked (see the top) */
    int h0 = s == 0 ? H_TIP : s <= 2 ? H_CALIPER : H_MAGSILVER, h1 = s == 0 ? H_TIP + 1 : s <= 2 ? H_CALIPER2 + 1 : NH;
    uint32_t so = rd32(mdl + MODEL_SOLID);
    if (!ok(so, SOLID_LM + 4u)) return;
    int n = (int8_t)MR8(so + SOLID_NLM);
    uint32_t t = MR32(so + SOLID_LM);
    if (n <= 0 || !ok(t, 8u * (uint32_t)n)) return;
    defaults();
    uint32_t car = rd32(ri + CRI_CAR), pl = rd32(G_PLAYER1), me = pl ? rd32(pl + 4u) : 0u;
    int mine = car && car == me, bad = 0, any = 0;
    for (int e = 0; e < n; e++){
        uint32_t h = MR32(t + 8u * (uint32_t)e), has = MR32(t + 8u * (uint32_t)e + 4u);
        for (int i = h0; i < h1; i++) if (h == g_hash[i]){
            uint32_t want = paint ? paint : g_def[i];
            any = 1;
            if (has != want){ bad = 1; W.last_h = h; W.last_has = has; W.last_want = want; }
        }
    }
    if (!any) return;
    int kind = s == 0 ? 0 : s <= 2 ? 1 : 2;
    W.n++; W.bad += (uint64_t)bad; W.mine += (uint64_t)mine; W.mine_bad += (uint64_t)(mine && bad);
    W.kn[kind]++; W.kbad[kind] += (uint64_t)bad;
}
/* "roam mp paint [on|off|reset]": the counts in a line; returns the draws with another car's paint */
uint64_t carpaint_report(char *out, int cap, const char *arg){
    if (arg && !strcmp(arg, "on")) g_carpaint_watch = 1;
    else if (arg && !strcmp(arg, "off")) g_carpaint_watch = 0;
    else if (arg && !strcmp(arg, "reset")) memset(&W, 0, sizeof W);
    snprintf(out, (size_t)cap, "%s, %s: %llu part draws checked (%llu of the player's car), %llu with another car's paint "
             "(%llu of the player's car); tips %llu/%llu, calipers %llu/%llu, rims %llu/%llu",
             g_carpaint_watch ? "watching" : "not watching", g_gameway > 0 ? "the game's way" : "each car its own paint",
             (unsigned long long)W.n, (unsigned long long)W.mine, (unsigned long long)W.bad, (unsigned long long)W.mine_bad,
             (unsigned long long)W.kbad[0], (unsigned long long)W.kn[0], (unsigned long long)W.kbad[1],
             (unsigned long long)W.kn[1], (unsigned long long)W.kbad[2], (unsigned long long)W.kn[2]);
    defaults();
    size_t at = strlen(out);
    snprintf(out + at, (size_t)cap - at, "; loaded with:");       /* each name's material (DEFAULT's hash 2c420d64: none of its own) */
    for (int i = 0; i < NH; i++){ at = strlen(out); snprintf(out + at, (size_t)cap - at, " %08x", ok(g_def[i], LM_HASH + 4u) ? MR32(g_def[i] + LM_HASH) : 0u); }
    if (W.bad){
        at = strlen(out);
        snprintf(out + at, (size_t)cap - at, "; the last: name %08x has %08x (hash %08x), its own %08x (hash %08x)", W.last_h,
                 W.last_has, ok(W.last_has, LM_HASH + 4u) ? MR32(W.last_has + LM_HASH) : 0u, W.last_want,
                 ok(W.last_want, LM_HASH + 4u) ? MR32(W.last_want + LM_HASH) : 0u);
    }
    return W.bad;
}
