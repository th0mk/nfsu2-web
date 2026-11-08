/* hqvinyls.c -- HQ vinyls: the cars' vinyls and doorlines at 1024x1024, as
 * AdrienShadow's "High Quality Vinyls & Doorlines (1024x Upscaled)"
 * (https://nfsmods.xyz/mod/8064) has them.  hq_vinyls = on
 * (NFSU2_HQ_VINYLS=1); off here, on from the page on computers.  Browser only:
 * the page does the painting (web/src/gpu/hqskin.ts), on a PC (GW_FEAT_HQSKIN).
 *
 * The game paints a car's skin on the CPU: base paint, its vinyl layers 0-3 and
 * the doorline, each picture through its _MASK, into a 512x512 texture
 * (DUMMY_SKIN<n>; free roam's own for its other cars), one target pixel per
 * layer pixel (0x611F10).  The mod makes every picture and the targets 1024 in
 * the game's files.  Here the game's files and its 512 skin stay as they are:
 * once the game has painted a skin, the recipe it painted from goes to the
 * page, which paints it again at 1024 from the mod's pictures
 * (HQVINYLS/CARS/<car>.hqv, tools/hqvinyls.py) with the same arithmetic and
 * shows that in the skin's place, until the game paints that texture again.
 * A car with a picture the pack lacks (Art Factory 28-34) keeps the game's.
 *
 * The recipe (0x611F10's argument, on 0x61D790's stack; its one caller is
 * 0x61DDF0): +0 the target's TextureInfo, +4 the base paint, +8 seven swatch
 * colours (the page takes those from the game's 512 skin), +0x24 up to 8
 * layers of 0x34 bytes, +0x1C4 their count.  A layer: +0 its picture's name
 * hash, +4 its TextureInfo, +8 its mask's, +0xC / +0x14 their pixels (A8R8G8B8,
 * locked for the call), +0x20 REMAP (coloured by +0x24's colours 0-2 by the
 * picture's R, G, B), +0x24 four colours.  Colours are R, G, B, A bytes until
 * 0x611F10 swaps them in place: read before it runs.  A layer without REMAP
 * is drawn as it is, alpha too (the skin's alpha is its gloss); the mod's
 * pictures have none (all 255), so the game's own alpha goes with the recipe.
 * DUMMY_SKIN1-6 keep the last recipe (0x89F050): the same one again is not
 * painted, nor sent. */
#include "cpu.h"
#include "guest.h"
#include "game.h"
#include "setting.h"
#include "web/gpu_proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RET_PAINT       0x0061ddf5u   /* 0x61DDF0's call of 0x611F10 returns here */
#define RCP_TARGET      0x000u        /* the recipe: the target's TextureInfo */
#define RCP_BASE        0x004u        /* ... the base paint */
#define RCP_LAYERS      0x024u        /* ... the layers */
#define RCP_COUNT       0x1c4u        /* ... their count */
#define RCP_RIDE        0x36cu        /* (0x61DDF0's frame) the ride, 0x61D790's argument: +0 its car type */
#define LAYER_SIZE      0x34u
#define L_HASH          0x00u
#define L_MASK_TI       0x08u
#define L_PIXELS        0x0cu
#define L_MASK_PIXELS   0x14u
#define L_REMAP         0x20u
#define L_COLOURS       0x24u
#define SIDE            GW_SKIN_HQ_SIDE
#define LOG_MAX         40            /* paints logged one by one */

/* gpu_web.c in the browser; the Mac paints nothing again */
__attribute__((weak)) int gw_hqskin_ok(void){ return 0; }
__attribute__((weak)) void gw_skin_hq(unsigned id, const GwSkinHq *r, const uint8_t *const *planes, unsigned nplanes){
    (void)id; (void)r; (void)planes; (void)nplanes;
}

static int g_on = -1;
static uint32_t g_serial;
static unsigned g_logged, g_sent, g_planes;

static int hq_on(void){
    if (g_on < 0){
        g_on = setting_bool("NFSU2_HQ_VINYLS", 0);
        if (g_on) fprintf(stderr, "[hqvinyls] on: the page paints the cars' skins again at 1024 (%s)\n",
                          gw_hqskin_ok() ? "it can" : "not this page: a phone, no BC, or not the browser");
    }
    return g_on && gw_hqskin_ok();
}

/* the car's texture name ("350Z"): its CarTypeInfo +0x20 */
static int car_name(uint32_t ride, char out[16]){
    uint32_t types = guest_rd32(G_CARTYPES), type = guest_rd32(ride);
    if (!types || type > 0x400u) return 0;
    uint32_t a = types + type * CARTYPE_SIZE + CARTYPE_TEXNAME;
    if (!guest_ok_bytes(a, 16)) return 0;
    memset(out, 0, 16);
    for (int k = 0; k < 15; k++){
        char ch = (char)MR8(a + (uint32_t)k);
        if (!ch) break;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) return 0;
        out[k] = ch;
    }
    return out[0] != 0;
}

/* a layer drawn as it is: its alpha where its mask draws it, one value or the game's 512 plane (malloc'd) */
static uint32_t layer_alpha(uint32_t px, uint32_t mpx, uint8_t **plane){
    const uint32_t n = SIDE * SIDE;
    int one = -1, many = 0;
    for (uint32_t i = 0; i < n && !many; i++){
        if (!MR8(mpx + 4u * i + 1u)) continue;            /* the mask's G: 0 = not drawn here */
        int a = MR8(px + 4u * i + 3u);
        if (one < 0) one = a; else if (a != one) many = 1;
    }
    if (!many) return one < 0 ? 255u : (uint32_t)one;
    uint8_t *p = malloc(n);
    if (!p) return 255u;
    for (uint32_t i = 0; i < n; i++) p[i] = MR8(px + 4u * i + 3u);
    *plane = p;
    return GW_SKIN_HQ_PLANE;
}

/* the recipe as the page wants it, before 0x611F10 swaps its colours; 0: not one to paint again */
static int recipe(uint32_t r, GwSkinHq *q, uint8_t **planes, unsigned *np){
    uint32_t ti = guest_rd32(r + RCP_TARGET), n = guest_rd32(r + RCP_COUNT);
    if (!ti || !guest_ok(ti, 0x7c) || MR16(ti + TI_WIDTH) != SIDE || MR16(ti + TI_HEIGHT) != SIDE) return 0;
    if (!n || n > GW_SKIN_HQ_LAYERS) return 0;                 /* no layer: nothing to paint sharper */
    memset(q, 0, sizeof *q);
    q->magic = GW_SKIN_HQ_MAGIC;
    if (!car_name(guest_rd32(r + RCP_RIDE), q->car)) return 0;
    q->base = MR32(r + RCP_BASE);
    q->n = n;
    for (uint32_t i = 0; i < n; i++){
        uint32_t L = r + RCP_LAYERS + i * LAYER_SIZE;
        GwSkinHqLayer *l = &q->layer[i];
        uint32_t mti = guest_rd32(L + L_MASK_TI), px = guest_rd32(L + L_PIXELS), mpx = guest_rd32(L + L_MASK_PIXELS);
        l->hash = MR32(L + L_HASH);
        l->mask = mti && guest_ok(mti + TI_HASH, 3) ? MR32(mti + TI_HASH) : 0;
        if (!l->hash || !l->mask) return 0;
        l->remap = MR32(L + L_REMAP) != 0;
        for (int k = 0; k < 4; k++) l->col[k] = MR32(L + L_COLOURS + 4u * (uint32_t)k);
        if (l->remap) continue;
        if (!guest_ok_bytes(px, SIDE * SIDE * 4u) || !guest_ok_bytes(mpx, SIDE * SIDE * 4u)) return 0;
        l->alpha = layer_alpha(px, mpx, &planes[*np]);
        if (l->alpha == GW_SKIN_HQ_PLANE) (*np)++;
    }
    q->serial = ++g_serial;
    return 1;
}

void sub_00611f10_orig(CPU *c);
void sub_00611f10(CPU *c){
    uint32_t r = MR32(c->esp + 4u);
    GwSkinHq q;
    uint8_t *planes[GW_SKIN_HQ_LAYERS] = { 0 };
    unsigned np = 0;
    int want = hq_on() && MR32(c->esp) == RET_PAINT && recipe(r, &q, planes, &np);
    sub_00611f10_orig(c);                                      /* the game's 512 skin, as always */
    if (want && c->eax){
        uint32_t ti = guest_rd32(r + RCP_TARGET), pl = guest_rd32(ti + TI_PLAT);
        uint32_t tex = pl ? guest_rd32(pl + PLAT_D3DTEX) : 0, id = tex ? guest_rd32(tex + D3DTEX_ID) : 0;
        if (id){
            gw_skin_hq(id, &q, (const uint8_t *const *)planes, np);
            g_sent++; g_planes += np;
            if (g_logged < LOG_MAX){
                g_logged++;
                fprintf(stderr, "[hqvinyls] skin t%u (0x%08x): %s, %u layer(s), %u alpha plane(s)%s\n", id,
                        MR32(ti + TI_HASH), q.car, q.n, np, g_logged == LOG_MAX ? "; no more of these lines" : "");
            }
        }
    }
    for (unsigned k = 0; k < np; k++) free(planes[k]);
}
