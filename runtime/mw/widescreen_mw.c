/* widescreen_mw.c - Most Wanted on a screen of another shape than its 4:3
 * modes (TRACKER M7-01..M7-06), done in the game's own space as
 * Underground 2's widescreen.c does it (L-093), at the code sites
 * ThirteenAG's NFSMostWanted.WidescreenFix patches (MIT), for speed.exe v1.3.
 *
 * The game keeps the 4:3 mode its options picked (its table at 0x6C27D0,
 * 640x480 to 1600x1200); com.c's res_pick makes the device the size the
 * page's canvas asks for (the menu's rows relabelled: 720p, 1080p, 1440p, the
 * screen's own, 4K) and hands the ratio k = game aspect / device aspect to
 * ws_set_mapping.  The renderer leaves every draw as the game made it
 * (ws_persp_* and ws_ui_* stay 1); the game's code is told the shape here:
 *
 *   FOV     Hor+ (Vert+ on an upright screen) in the projection setup
 *           0x6CF400 for the player's view (id 1) and its road reflection
 *           (id 4): P00 = 1/tan(fov/2) just made at 0x6CF566 is scaled by k,
 *           P11 just made at 0x6CF5DC divided by k (narrower than 4:3).  The
 *           culling frustum is built from the same matrix after it.  The
 *           mirror (3), shadow views (8, 9) and the reflections above 10 keep
 *           the game's projection; the x87 stack is not rewritten, only the
 *           one value about to be stored.  The shadow map's camera, fitted to
 *           the player's view (P00 at 0x6E46B0 and 0x6E4830), widens with it,
 *           so the shadows reach the screen's sides.
 *   HUD     the front end and the HUD are drawn in 640x480 units the game
 *           maps to clip space itself (0x6D12C0: x * [0x8AF9A4] - 1, y *
 *           [0x8AF9A0] - 1): the scale shrinks to keep 640x480 4:3 in the
 *           middle, and the screen centre the packages are placed around
 *           (320 at 0x894B40, read by 33 sites, and the (320, 240) origin the
 *           seven HUD and front-end renders push) moves to the middle of the
 *           wider space; narrower than 4:3 the same is done in y.  The
 *           rear-view mirror's quad (190..450 x 19..89, 0x6E70BC..0x6E70F8)
 *           moves with it; the screen's size in HUD units (640 at 0x8AE8F8,
 *           480 at 0x8AF2C0: the mouse 0x6C9C45, the world-to-HUD markers
 *           0x6D0581, Autosculpt) and the arrest blur's step (1/640 at
 *           0x8AFA08) follow.  On a 16:9 or wider screen with hud = wide the
 *           game's own widescreen HUD (its WIDESCREEN_MODE packages,
 *           FEngHud::SetWideScreenMode 0x5696C0 and the minimap's
 *           AdjustForWidescreen) is used: the four reads of the console's
 *           widescreen flag [[0x91CF90]+0x10]+0x34 (HUD 0x5696CB and
 *           0x57CB82, the EA Trax pop-up 0x58D883, the wrong-way sign
 *           0x56885A) see it set; the flag itself is never written (it is
 *           saved with the profile).
 *   movie   the movie quad (0x59A029..0x59A038): -0.5..0.5 goes through the
 *           front end's scale, so it is the 4:3 middle (fit) already; fill
 *           and stretch widen it to the screen.
 *   labels  the options' six resolution rows (label hashes at 0x51B7A0,
 *           looked up by 0x57E920) show com.c's row names.
 *
 * Options (read once, the page's settings): NFSU2_HUD=wide|43 (hud),
 * NFSU2_MOVIE=fit|fill|stretch (movie_framing).  NOWIDE=1 (com.c) keeps the
 * game's own table and labels.  The native row's size (render, max_pixels)
 * is wsopts.c's, as for every game. */
#include "cpu.h"
#include "widescreen.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

long autotest_frame(void);
int  com_res_label(int row, char *buf, unsigned cap);  /* com.c: the resolution row's name, 0 = the game's own */

enum { HUD_43, HUD_WIDE };
enum { MOV_FIT, MOV_FILL, MOV_STRETCH };

static int   g_init;
static int   g_hud = HUD_WIDE, g_movie = MOV_FIT;
static float g_k = 1.f;                 /* game aspect / device aspect (com.c res_pick) */
static float g_hk = 1.f;                /* 4:3 / device aspect: the HUD's own factor */
static float g_cx = 320.f, g_cy = 240.f;    /* the screen's centre in HUD units */
static int   g_wide_hud;                /* the game's widescreen HUD packages in use */
static unsigned long g_hits[12];

static int opt_is(const char *v, const char *s){ return v && !strcasecmp(v, s); }
static void mws_init(void){
    if (g_init) return;
    g_init = 1;
    const char *e;
    if ((e = getenv("NFSU2_HUD")) && *e) g_hud = (opt_is(e, "43") || opt_is(e, "4:3")) ? HUD_43 : HUD_WIDE;
    if ((e = getenv("NFSU2_MOVIE")) && *e)
        g_movie = opt_is(e, "fill") ? MOV_FILL : opt_is(e, "stretch") ? MOV_STRETCH : MOV_FIT;
    fprintf(stderr, "ws: mw hud=%s movie=%s\n", g_hud == HUD_WIDE ? "wide" : "4:3",
            g_movie == MOV_FIT ? "fit" : g_movie == MOV_FILL ? "fill" : "stretch");
}

/* The game's mode now (its table, RESOLUTION_INDEX [0x90181C]): its aspect, 4:3 but for 1280x1024 */
static float game_aspect(void){
    static const uint16_t tw[6] = {640, 800, 1024, 1280, 1280, 1600}, th[6] = {480, 600, 768, 960, 1024, 1200};
    uint32_t i = MR32(0x0090181cu);
    return i < 6u ? (float)tw[i] / (float)th[i] : 4.f / 3.f;
}

/* The HUD's mapping for g_k: its scale and the screen's centre, written where the game reads them */
static void hud_apply(void){
    float hk = g_k * (4.f / 3.f) / game_aspect();
    if (!(hk > 0.05f && hk < 20.f)) hk = 1.f;
    g_hk = hk;
    float sx = 1.f / 320.f, sy = 1.f / 240.f;
    g_cx = 320.f; g_cy = 240.f;
    if (hk < 1.f){ sx = hk / 320.f; g_cx = 320.f / hk; }        /* wider: 640/hk units across */
    else if (hk > 1.f){ sy = 1.f / (240.f * hk); g_cy = 240.f * hk; }  /* narrower: 480*hk units down */
    MRF32(0x008af9a4u) = sx;            /* the front end's x scale (1/320) */
    MRF32(0x008af9a0u) = sy;            /* its y scale (1/240) */
    MRF32(0x00894b40u) = g_cx;          /* the screen's centre x (320) */
    MRF32(0x008ae8f8u) = 2.f * g_cx;    /* the screen's width in HUD units (640) */
    MRF32(0x008af2c0u) = 2.f * g_cy;    /* its height (480) */
    MRF32(0x008afa08u) = (hk < 1.f ? hk : 1.f) / 640.f;    /* the arrest blur's x step (1/640) */
    /* the game's 16:9 HUD packages need 16:9's room; 16:10 keeps the 4:3 HUD in the middle */
    g_wide_hud = g_hud == HUD_WIDE && hk < 0.76f;
}

void ws_set_mapping(float k){
    mws_init();
    if (!(k > 0.05f && k < 20.f)) k = 1.f;
    int changed = k != g_k;
    g_k = k;
    hud_apply();
    if (changed)
        fprintf(stderr, "ws: game/device aspect %.4f, HUD %.4f: centre (%.1f, %.1f), %s HUD\n", (double)k,
                (double)g_hk, (double)g_cx, (double)g_cy, g_wide_hud ? "the game's widescreen" : "the 4:3");
}
/* the renderer changes nothing: the game draws for the screen's shape itself */
float ws_persp_x(void){ return 1.f; }
float ws_persp_y(void){ return 1.f; }
float ws_ui_x(void){ return 1.f; }
float ws_ui_y(void){ return 1.f; }

/* The movie quad's four slots, pushed for 0x591460 (left, top, right, bottom): -0.5..0.5 of the front end's 4:3
 * space, which hud_apply's scale keeps in the middle of the screen.  So fit (the 4:3 picture whole, bars at the
 * sides, or above and below) is the game's quad; fill covers the screen keeping the aspect (cropped); stretch
 * covers it ignoring the aspect.  sx, sy: the screen in units of that space. */
static void movie_quad(uint32_t sp){
    if (fabsf(MRF32(sp) + 0.5f) > 1e-4f || fabsf(MRF32(sp + 8u) - 0.5f) > 1e-4f) return;
    float sx = g_hk < 1.f ? 1.f / g_hk : 1.f, sy = g_hk > 1.f ? g_hk : 1.f, hx = 0.5f, hy = 0.5f;
    if (g_movie == MOV_FILL){ float s = fmaxf(sx, sy); hx = hy = 0.5f * s; }
    else if (g_movie == MOV_STRETCH){ hx = 0.5f * sx; hy = 0.5f * sy; }
    MRF32(sp) = -hx; MRF32(sp + 4u) = -hy; MRF32(sp + 8u) = hx; MRF32(sp + 12u) = hy;
}

/* The console's widescreen flag, read at four sites: a site's byte shows g_wide_hud while that one instruction
 * reads it, and is put back right after (the flag is part of the saved profile) */
static uint32_t g_flag_at;
static uint8_t  g_flag_was;
static void flag_show(uint32_t a){
    if (!a || !mem_is_committed(a)) return;
    g_flag_at = a; g_flag_was = MR8(a);
    MR8(a) = (uint8_t)g_wide_hud;
}
static void flag_back(void){
    if (g_flag_at) MR8(g_flag_at) = g_flag_was;
    g_flag_at = 0;
}

/* The seven renders that push the screen centre (320, 240, 0, 1) as their origin: the hook is on the
 * instruction after the 240's store, the x at [esp+off] */
static const struct { uint32_t va; uint16_t off; } g_origin[7] = {
    { 0x0056fee3u, 0x19cu }, { 0x00584f05u, 0x90u }, { 0x00599e8fu, 0x84u }, { 0x0059a136u, 0xc4u },
    { 0x0059a5c1u, 0x94u }, { 0x0059a854u, 0x94u }, { 0x005a44d8u, 0x64u },
};

int mw_ws_hook(CPU *c, uint32_t va){
    switch (va){
    /* VIEW_PROJECTION 0x6CF400 (thiscall, the eView at [esp0+0x20]): P00 = 1/tan(fov/2) in ST0 before its
     * store at 0x6CF56C (two pushes deep), P11 = 1/tan(fov*aspect*1.15/2) before 0x6CF5E2 (four deep) */
    case HOOK_SITE(0x006cf56c):
    case HOOK_SITE(0x006cf5e2): {
        int y = va == 0x006cf5e2u;
        g_hits[y]++;
        uint32_t view = MR32(c->esp + (y ? 0x30u : 0x28u));
        uint32_t id = mem_is_committed(view + 4u) ? MR32(view + 4u) : 0u;
        if (id != 1u && id != 4u) return 1;
        double before = ST(0);
        if (!y && g_k < 1.f) ST(0) *= (double)g_k;
        else if (y && g_k > 1.f) ST(0) /= (double)g_k;
        static unsigned shown[2];
        if (shown[y] < 4 && ST(0) != before){
            shown[y]++;
            uint32_t vp = MR32(view + 0x60u);
            fprintf(stderr, "ws: view %u %s %.4f -> %.4f (view size %ux%u)\n", id, y ? "P11" : "P00", before, ST(0),
                    mem_is_committed(vp + 0x1bu) ? MR32(vp + 0x14u) : 0u, mem_is_committed(vp + 0x1bu) ? MR32(vp + 0x18u) : 0u);
        }
        return 1; }
    /* the origin (320, 240) of the HUD and front-end renders */
    case HOOK_SITE(0x0056fee3): case HOOK_SITE(0x00584f05): case HOOK_SITE(0x00599e8f): case HOOK_SITE(0x0059a136):
    case HOOK_SITE(0x0059a5c1): case HOOK_SITE(0x0059a854): case HOOK_SITE(0x005a44d8):
        for (int i = 0; i < 7; i++) if (g_origin[i].va == va){
            g_hits[2]++;
            MRF32(c->esp + g_origin[i].off) = g_cx;
            MRF32(c->esp + g_origin[i].off + 4u) = g_cy;
        }
        return 1;
    /* the rear-view mirror's quad: x 450, 190, 190, 450 at [esp+0x70/0x80/0x90/0xA0], y 19, 19, 89, 89 after them */
    case HOOK_SITE(0x006e710e):
        g_hits[3]++;
        for (uint32_t o = 0x70u; o <= 0xa0u; o += 0x10u){
            MRF32(c->esp + o) += g_cx - 320.f;
            MRF32(c->esp + o + 4u) += g_cy - 240.f;
        }
        return 1;
    /* the shadow map's camera (two fits of the player's view): its P00 = 1/tan(fov/2) before the stores at
     * 0x6E46B6 and 0x6E4836 */
    case HOOK_SITE(0x006e46b6):
    case HOOK_SITE(0x006e4836):
        g_hits[4]++;
        if (g_k < 1.f) ST(0) *= (double)g_k;
        return 1;
    /* the widescreen flag's four reads: mov al,[ecx+34] (SetWideScreenMode), cmp dl,[ebp+34] (the HUD's
     * update), mov al,[eax+34] (EA Trax), cmp [eax+34],cl (the wrong-way sign); each followed by its put-back */
    case HOOK_SITE(0x005696cb): g_hits[5]++; flag_show(c->ecx + 0x34u); return 1;
    case HOOK_SITE(0x0057cb82): g_hits[5]++; flag_show(c->ebp + 0x34u); return 1;
    case HOOK_SITE(0x0058d883): g_hits[5]++; flag_show(c->eax + 0x34u); return 1;
    case HOOK_SITE(0x0056885a): g_hits[5]++; flag_show(c->eax + 0x34u); return 1;
    case HOOK_SITE(0x005696ce): case HOOK_SITE(0x0057cb85): case HOOK_SITE(0x0058d886): case HOOK_SITE(0x0056885d):
        flag_back();
        return 1;
    /* the movie quad: its four extents are the last four pushes before `mov ecx, ebx; call 0x591460` */
    case HOOK_SITE(0x0059a03d):
        g_hits[6]++;
        movie_quad(c->esp);
        return 1;
    default:
        return 0;
    }
}

/* The localised string lookup 0x57E920 (cdecl: wchar_t *out, size in characters, hash; al = found): the six
 * resolution rows' labels (0x51B7A0's hashes, rows 0..5) as com.c names the rows */
static const uint32_t g_res_hash[6] = { 0xdccbd59eu, 0x6d6bb416u, 0xa7470774u, 0x4ef21b32u, 0x2d31055au, 0x76667b12u };
void sub_0057e920_orig(CPU *c);
void sub_0057e920(CPU *c){
    uint32_t out = MR32(c->esp + 4u), cap = MR32(c->esp + 8u), hash = MR32(c->esp + 12u);
    char label[32];
    for (int i = 0; i < 6; i++){
        if (hash != g_res_hash[i]) continue;
        if (!com_res_label(i, label, sizeof label)) break;
        uint32_t n = (uint32_t)strlen(label);
        if (!cap || n + 1u > cap || !mem_is_committed(out) || !mem_is_committed(out + 2u * n + 1u)) break;
        for (uint32_t j = 0; j <= n; j++) MR16(out + 2u * j) = (uint16_t)(uint8_t)label[j];
        g_hits[7]++;
        c->eax = (c->eax & ~0xffu) | 1u;
        POP32(c->eip);
        return;
    }
    sub_0057e920_orig(c);
}

void ws_report(void){
    if (!g_init) return;
    fprintf(stderr, "ws: mw hooks fov=%lu/%lu origin=%lu mirror=%lu shadow=%lu wideflag=%lu movie=%lu labels=%lu"
            " (k=%.4f, HUD %.4f, %s HUD)\n", g_hits[0], g_hits[1], g_hits[2], g_hits[3], g_hits[4], g_hits[5],
            g_hits[6], g_hits[7], (double)g_k, (double)g_hk, g_wide_hud ? "wide" : "4:3");
}
