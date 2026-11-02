/* headlights.c -- other cars' headlights light the road too (web renderer).
 *
 * The game lights the road with one projector, view 9: a camera on the
 * player's car (its drawn position +0x50 and 0.76 m up, the car's heading
 * pitched 5.7 degrees down, its right axis level; FOV 143 degrees, aspect
 * 0.6, near 1 m, far 100 m), whose view * projection sub_005c7ec0 builds.
 * Each view render (0x5CAAD0) marks the solids view 9 sees as lit (0x5D6D20
 * over view 9's list with 0x488B90: bit 31 of the instance's +0x18, cleared
 * as each is drawn, 0x5D6AD0) and draws those with the worldheadlight
 * effects.  Their vertex shaders put the light coordinate q = pos *
 * HEADLIGHTWVP in oT1 and 0.005 q.w + 0.5 in oT2 (vs_13, vs_14), where
 * HEADLIGHTWVP = X * VP9 * B for every draw (0x5BDAA0: X the draw's matrix,
 * B a translation by (0.5, 0.5), the globals at 0x80052C); their pixel
 * shaders add t1(q.xy / q.w) * t2(oT2) * HEADLIGHTCOLOR to the vertex light
 * before the texture multiply (ps 4 and 6).
 *
 * Other players' cars get the same light: the friends' in free roam, and in
 * an online or LAN race every car but traffic.  Car k's projector is the
 * player's moved by the rigid transform from the player's car to car k (C = a
 * car's drawn matrix: rotation +0xD0, position +0x50), Rel_k = inv(C_k) *
 * C_p, so its coordinate is q_k = q * T_k with T_k = inv(VP9 B) * Rel_k * VP9
 * B, whatever X is: one 4x4 per car and frame, nothing per draw.  At each
 * view render (the override of 0x5D6D20 below):
 *  - the nearest GW_HL_MAX of those cars in reach get T_k and their own
 *    headlight colour ([[car+0x554]+0x380], read as the game reads the
 *    player's) at 60 %, published to the renderer (gw_headlights: the
 *    frame's block that the lit pixel shaders loop over, vsemu_wgsl.inc);
 *  - the solids of the view being drawn that their frusta (Rel_k * VP9)
 *    touch are marked lit too, so they are drawn with the lit effects.
 * The light goes through walls and cars as the player's own does (it has no
 * shadow); from the driver's seat that was never seen.  But it also went
 * down through the road the car is on: a car on an overpass lit the street
 * under it.  So the light stops below the car's own road: T_k's z column
 * (the projector's depth, which the shaders never read) carries instead the
 * height of the lit point above the plane through car k's position across
 * its up axis (rotation row 2), in metres; the shader fades the light out
 * from 2 m below that plane, more the farther ahead (a road dipping or the
 * car pitching), gone 1.5 m lower (vsemu_wgsl.inc W_HL).  Under an
 * overpass the street is 6 m or more below the car on it.
 * Only the web renderer draws the extra light (the Mac's translator has no
 * such term): elsewhere the override is the game's function.
 *   NFSU2_HEADLIGHTS=players (default) | all (every other car: AI racers and
 *   traffic too) | off | self (the player's own light once more: a test of
 *   the term), optionally :GAIN (0.6), and for self @DZ: that copy DZ metres
 *   higher (a car on a bridge overhead, a test of the cut); the script
 *   command "headlights MODE" sets it for an A/B within one run. */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sub_005d6d20_orig(CPU *c);

#ifdef __wasm__
#include "web/gpu_proto.h"
int photo_hides(uint32_t ri);                    /* photo.c: a car photo mode does not draw */

#define VIEW9       0x008331d0u   /* eView 9 (0x48B1E0(9): 0x832DE0 + 9 * 0x70): the player's headlight */
#define CUR_VIEW     0x00870848u   /* the viewport (eView +0x58) being rendered (0x5CAAD0); view 9 shares view 1's */
#define LIT_CB       0x00488b90u   /* 0x5D6D20's callback that marks a solid lit */
#define HL_ENABLE    0x00870cc4u   /* g_CarHeadlightEnable */
#define HL_BIAS      0x0080052cu   /* B: x, y offsets after the projection */
#define G_PLAYERS    0x008900b4u   /* Player*[]: +4 car (0x5BDAA0 takes the colour from [0]) */
#define G_WORLD      0x00890080u   /* World*: its car list (car +0 next) has the World as head and end */
#define G_GAMEMODE   0x00850078u   /* 8 online, 0x200 LAN */
#define GM_NETWORK   0x208u
#define CAR_DRIVER   0x24u         /* 0x5EBA30's type: 1 the player, 3 an AI racer, 4 traffic, 0 none */
#define DRIVER_TRAFFIC 4u
#define CAR_POS      0x50u         /* drawn position */
#define CAR_ROT      0xd0u         /* drawn rotation, 3 rows of 16 bytes */
#define CAR_ACTIVE   0x550u
#define CAR_RENDER   0x554u        /* CarRenderInfo*: +0x380 the headlight colour */
#define CAR_BLINKOUT 0x998u
#define REACH_M      160.0         /* cars this far from the player's: a light pool up to 100 m ahead */

typedef struct { double m[4][4]; } M4;
void gw_headlights(const GwHL *blk);            /* runtime/web/gpu_web.c */

enum { HL_OFF, HL_PLAYERS, HL_ALL, HL_SELF };
static int  g_mode = -1;                        /* HL_SELF: the player's own light once more (a test of the term) */
static M4   g_vp9; static int g_have_vp9;       /* view 9's view * projection, as last built */
static unsigned long g_vp9_sets;
static float g_ramp[2] = { 0.005f, 0.5f };      /* the lit vertex shaders' falloff: oT2.x = s q.w + o */
static int  g_n;                                /* lights this frame */
static double g_pl[GW_HL_MAX][6][4];            /* their frusta (world planes, inside >= 0) */
static GwHL g_blk;
static unsigned long g_marked, g_frames_lit;

/* their light is GAIN times the player's (the colour): 0.6 keeps the road
 * readable where several pools overlap (picked from rendered variants,
 * 2026-10-20); "MODE:GAIN" sets another */
static float g_gain = 0.6f;
static float g_lift;                            /* HL_SELF: the copy this many metres higher */
static int mode_of(const char *s){
    char m[16] = "";
    if (s){ size_t n = strcspn(s, ":@"); if (n < sizeof m){ memcpy(m, s, n); m[n] = 0; }
            if (s[n] == ':'){ float g = (float)atof(s + n + 1); if (g >= 0.f && g <= 4.f) g_gain = g; }
            const char *at = strchr(s, '@');
            g_lift = at ? (float)atof(at + 1) : 0.f; }
    if (!m[0] || !strcmp(m, "players") || !strcmp(m, "on") || !strcmp(m, "1")) return HL_PLAYERS;
    if (!strcmp(m, "all")) return HL_ALL;
    if (!strcmp(m, "off") || !strcmp(m, "0")) return HL_OFF;
    if (!strcmp(m, "self")) return HL_SELF;
    return -1;
}
static int hl_on(void){
    if (g_mode < 0){
        g_mode = mode_of(getenv("NFSU2_HEADLIGHTS"));
        if (g_mode < 0) g_mode = HL_PLAYERS;
    }
    return g_mode;
}
/* players: the friends' cars in free roam; in an online or LAN game every
 * car but traffic (those races have no AI racers) */
int roam_game_friend_car(uint32_t car);
static int wanted(uint32_t car, uint32_t me){
    if (g_mode == HL_SELF) return car == me;
    if (car == me) return 0;
    if (g_mode == HL_ALL) return 1;
    if (MR32(G_GAMEMODE) & GM_NETWORK) return MR32(car + CAR_DRIVER) != DRIVER_TRAFFIC;
    return roam_game_friend_car(car);
}

static void m4_mul(M4 *r, const M4 *a, const M4 *b){
    M4 t;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++){
            double s = 0;
            for (int k = 0; k < 4; k++) s += a->m[i][k] * b->m[k][j];
            t.m[i][j] = s;
        }
    *r = t;
}
/* Gauss-Jordan with partial pivoting; 0 when singular */
static int m4_inv(M4 *r, const M4 *a){
    double w[4][8];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++){ w[i][j] = a->m[i][j]; w[i][4 + j] = i == j; }
    for (int c = 0; c < 4; c++){
        int p = c;
        for (int i = c + 1; i < 4; i++) if (__builtin_fabs(w[i][c]) > __builtin_fabs(w[p][c])) p = i;
        if (__builtin_fabs(w[p][c]) < 1e-12) return 0;
        if (p != c) for (int j = 0; j < 8; j++){ double t = w[c][j]; w[c][j] = w[p][j]; w[p][j] = t; }
        double d = 1.0 / w[c][c];
        for (int j = 0; j < 8; j++) w[c][j] *= d;
        for (int i = 0; i < 4; i++){
            if (i == c || w[i][c] == 0) continue;
            double f = w[i][c];
            for (int j = 0; j < 8; j++) w[i][j] -= f * w[c][j];
        }
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r->m[i][j] = w[i][4 + j];
    return 1;
}
static int car_ok(uint32_t car){
    return car && mem_is_committed(car) && mem_is_committed(car + CAR_BLINKOUT + 3u);
}
/* a car's drawn matrix (row vectors: world = local * C) */
static void car_m4(uint32_t car, M4 *C){
    for (int r = 0; r < 3; r++){
        for (int c = 0; c < 3; c++) C->m[r][c] = MRF32(car + CAR_ROT + 16u * (uint32_t)r + 4u * (uint32_t)c);
        C->m[r][3] = 0;
    }
    for (int c = 0; c < 3; c++) C->m[3][c] = MRF32(car + CAR_POS + 4u * (uint32_t)c);
    C->m[3][3] = 1;
}
/* inverse of a rigid C: rotation transposed */
static void rigid_inv(M4 *r, const M4 *C){
    for (int i = 0; i < 3; i++){ for (int j = 0; j < 3; j++) r->m[i][j] = C->m[j][i]; r->m[i][3] = 0; }
    for (int j = 0; j < 3; j++){
        double s = 0; for (int k = 0; k < 3; k++) s += C->m[3][k] * C->m[j][k];
        r->m[3][j] = -s;
    }
    r->m[3][3] = 1;
}
/* clip = v * VP (D3D: -w <= x, y <= w, 0 <= z <= w) -> 6 world planes */
static void frustum(double pl[6][4], const M4 *vp){
    for (int r = 0; r < 4; r++){
        double c0 = vp->m[r][0], c1 = vp->m[r][1], c2 = vp->m[r][2], c3 = vp->m[r][3];
        pl[0][r] = c3 + c0; pl[1][r] = c3 - c0; pl[2][r] = c3 + c1; pl[3][r] = c3 - c1;
        pl[4][r] = c2;      pl[5][r] = c3 - c2;
    }
}

/* sub_005c7ec0 (overrides.c) has built view `id`'s matrices at plat: view
 * +0, projection +0x40, their product +0x80 */
void headlights_view(uint32_t id, const float *plat){
    if (id != 9u || !plat) return;
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) g_vp9.m[r][c] = plat[32 + 4 * r + c];
    g_have_vp9 = 1; g_vp9_sets++;
}
/* com.c: a vertex shader with the headlight projection (vs_light_creg) */
void headlights_vs_ramp(float s, float o){ g_ramp[0] = s; g_ramp[1] = o; }

/* the light set, made at each view render (a few times a frame: a few
 * matrix products per car) */
static void make_lights(void){
    g_n = 0;
    memset(&g_blk, 0, sizeof g_blk);
    uint32_t pl = MR32(G_PLAYERS), me = pl && mem_is_committed(pl + 7u) ? MR32(pl + 4u) : 0, w = MR32(G_WORLD);
    if (!g_have_vp9 || !car_ok(me) || !w || !mem_is_committed(w + 3u)) goto out;
    M4 Cp, B = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}}, M, Mi;
    car_m4(me, &Cp);
    B.m[3][0] = MRF32(HL_BIAS); B.m[3][1] = MRF32(HL_BIAS + 4u);
    m4_mul(&M, &g_vp9, &B);
    if (!m4_inv(&Mi, &M)) goto out;
    /* the nearest cars in reach */
    struct { uint32_t car; double d2; } near[GW_HL_MAX];
    int nn = 0, guard = 0;
    for (uint32_t car = MR32(w); car && car != w && guard < 256; car = MR32(car), guard++){
        if (!car_ok(car)) break;
        if (!wanted(car, me) || !MR8(car + CAR_ACTIVE) || MR8(car + CAR_BLINKOUT)) continue;
        uint32_t ri = MR32(car + CAR_RENDER);
        if (!ri || !mem_is_committed(ri + 0x383u) || photo_hides(ri)) continue;      /* (photo mode hides it: no light) */
        double d2 = 0;
        for (int k = 0; k < 3; k++){ double d = MRF32(car + CAR_POS + 4u * (uint32_t)k) - Cp.m[3][k]; d2 += d * d; }
        if (!(d2 < REACH_M * REACH_M)) continue;
        int at = nn < GW_HL_MAX ? nn++ : GW_HL_MAX;
        if (at == GW_HL_MAX){ if (d2 >= near[GW_HL_MAX - 1].d2) continue; at = GW_HL_MAX - 1; }
        while (at > 0 && near[at - 1].d2 > d2){ near[at] = near[at - 1]; at--; }
        near[at].car = car; near[at].d2 = d2;
    }
    for (int i = 0; i < nn; i++){
        uint32_t car = near[i].car, col = MR32(MR32(car + CAR_RENDER) + 0x380u);
        M4 Ck, Cki, Rel, VPk, T;
        car_m4(car, &Ck);
        if (g_mode == HL_SELF) Ck.m[3][2] += g_lift;
        rigid_inv(&Cki, &Ck);
        m4_mul(&Rel, &Cki, &Cp);                         /* world -> world: car k's frame onto the player's */
        m4_mul(&VPk, &Rel, &g_vp9);
        frustum(g_pl[g_n], &VPk);
        m4_mul(&T, &Mi, &Rel); m4_mul(&T, &T, &M);
        /* z column: the height above car k's road plane, (X - P_k) . U_k
         * for the world point X = q * Mi, so the column Mi * (U_k, -P_k . U_k) */
        double g[4] = { Ck.m[2][0], Ck.m[2][1], Ck.m[2][2], 0 };
        for (int a = 0; a < 3; a++) g[3] -= Ck.m[3][a] * Ck.m[2][a];
        for (int r = 0; r < 4; r++){
            double s = 0; for (int a = 0; a < 4; a++) s += Mi.m[r][a] * g[a];
            T.m[r][2] = s;
        }
        GwHLight *L = &g_blk.l[g_n];
        for (int j = 0; j < 4; j++) for (int r = 0; r < 4; r++) L->t[4 * j + r] = (float)T.m[r][j];   /* columns */
        L->col[0] = g_gain * (float)(col & 0xffu) / 255.f; L->col[1] = g_gain * (float)((col >> 8) & 0xffu) / 255.f;
        L->col[2] = g_gain * (float)((col >> 16) & 0xffu) / 255.f; L->col[3] = 0;
        g_n++;
    }
out:
    g_blk.n = (uint32_t)g_n; g_blk.s = g_ramp[0]; g_blk.o = g_ramp[1];
    gw_headlights(&g_blk);
    if (g_n) g_frames_lit++;
    { static int last = -1, shown;
      if (g_n != last && shown < 40){
          shown++; last = g_n;
          fprintf(stderr, "[headlights] %d other cars' lights (s %.4f o %.3f)", g_n, (double)g_ramp[0], (double)g_ramp[1]);
          for (int i = 0; i < g_n; i++)
              fprintf(stderr, "%s car 0x%08x (driver %u) at %.1f m, colour %.2f %.2f %.2f", i ? ";" : ":", near[i].car,
                      MR32(near[i].car + 0x24u), __builtin_sqrt(near[i].d2),
                      (double)g_blk.l[i].col[0], (double)g_blk.l[i].col[1], (double)g_blk.l[i].col[2]);
          fprintf(stderr, "\n");
      } }
}

/* Mark the solids of the view being drawn that a light touches: the entries
 * of its slot in the visibility lists `list` (0x5D6D20's this: slots of 0x90
 * bytes from +0, their count at +0x6C0; slot +0x40 the eView, +0x48/+0x4C
 * the entries, 12 bytes each: model | flags, matrix or 0, instance), as
 * 0x5D6AD0 walks them.  The view being drawn is the one whose viewport
 * (+0x58) is [0x870848] (0x5CAAD0 sets it), other than view 9, which shares
 * view 1's.  A model's meshes (solid [[model+0xC]]: +0x10 count, +0x14 0x3C
 * bytes each) have their box at +0 (min) and +0x10 (max), in the model's
 * frame (0x5C5930).  In a race about 150 entries, 85 lit by the player's
 * light already, ~18 a frame lit for the AI racers (quick race, mode all). */
static unsigned long g_mk[8];   /* calls, no slot, entries, already lit, no solid, no meshes, tested, (spare) */
static void mark(uint32_t list){
    uint32_t view = MR32(CUR_VIEW);
    int ns = (int)MR32(list + 0x6c0u);
    g_mk[0]++;
    if (!view || ns <= 0 || ns > 64){ g_mk[1]++; return; }
    uint32_t slot = 0;
    for (int k = 0; k < ns && !slot; k++){
        uint32_t s = list + 0x90u * (uint32_t)k, v = MR32(s + 0x40u);
        if (v && v != VIEW9 && mem_is_committed(v + 0x5bu) && MR32(v + 0x58u) == view) slot = s;
    }
    if (!slot){ g_mk[1]++; return; }
    uint32_t e = MR32(slot + 0x48u), end = MR32(slot + 0x4cu);
    if (end < e || end - e > 12u * 65536u){ g_mk[1]++; return; }
    for (; e < end; e += 12u){
        g_mk[2]++;
        uint32_t model = MR32(e) & ~3u, mx = MR32(e + 4u), inst = MR32(e + 8u);
        if (!model || !inst || !mem_is_committed(inst + 0x1bu) || (MR8(inst + 0x1bu) & 0x80u)){ g_mk[3]++; continue; }
        uint32_t sp = MR32(model + 0xcu), solid = sp ? MR32(sp) : 0;
        if (!solid || !mem_is_committed(solid + 0x17u)){ g_mk[4]++; continue; }
        uint32_t nm = MR32(solid + 0x10u), mesh = MR32(solid + 0x14u);
        if (!nm || nm > 4096u || !mem_is_committed(mesh) || !mem_is_committed(mesh + 0x3cu * nm - 1u)){ g_mk[5]++; continue; }
        g_mk[6]++;
        double lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 };
        for (uint32_t q = 0; q < nm; q++, mesh += 0x3cu)
            for (int a = 0; a < 3; a++){
                double l = MRF32(mesh + 4u * (uint32_t)a), h = MRF32(mesh + 0x10u + 4u * (uint32_t)a);
                if (l < lo[a]) lo[a] = l;
                if (h > hi[a]) hi[a] = h;
            }
        double c[3], ext[3];
        for (int a = 0; a < 3; a++){ c[a] = 0.5 * (lo[a] + hi[a]); ext[a] = 0.5 * (hi[a] - lo[a]); }
        if (!(ext[0] >= 0 && ext[1] >= 0 && ext[2] >= 0)) continue;
        if (mx && mem_is_committed(mx + 0x3fu)){            /* the box in the world: centre * M, |R| * extent */
            double wc[3], we[3];
            for (int j = 0; j < 3; j++){
                wc[j] = MRF32(mx + 0x30u + 4u * (uint32_t)j); we[j] = 0;
                for (int a = 0; a < 3; a++){
                    double r = MRF32(mx + 16u * (uint32_t)a + 4u * (uint32_t)j);
                    wc[j] += c[a] * r; we[j] += ext[a] * __builtin_fabs(r);
                }
            }
            memcpy(c, wc, sizeof c); memcpy(ext, we, sizeof ext);
        }
        for (int k = 0; k < g_n; k++){
            int in = 1;
            for (int p = 0; p < 6 && in; p++){
                const double *P = g_pl[k][p];
                double d = P[0] * c[0] + P[1] * c[1] + P[2] * c[2] + P[3]
                         + __builtin_fabs(P[0]) * ext[0] + __builtin_fabs(P[1]) * ext[1] + __builtin_fabs(P[2]) * ext[2];
                if (d < 0) in = 0;
            }
            if (in){ MR8(inst + 0x1bu) |= 0x80u; g_marked++; break; }
        }
    }
}

void headlights_report(void){
    if (g_mode > 0) fprintf(stderr, "[headlights] view 9 set up %lu times; view renders with other cars' light %lu, "
                            "solids lit for them %lu (marks %lu, no slot %lu, entries %lu, lit already %lu, no solid %lu, "
                            "no meshes %lu, tested %lu)\n", g_vp9_sets, g_frames_lit, g_marked,
                            g_mk[0], g_mk[1], g_mk[2], g_mk[3], g_mk[4], g_mk[5], g_mk[6]);
}
int headlights_set(const char *mode){
    int m = mode_of(mode);
    if (m < 0) return 0;
    g_mode = m;
    return 1;
}
#else
int headlights_set(const char *mode){ (void)mode; return 0; }
#endif

/* 0x5D6D20, thiscall (eView*, callback), ret 8: the callback over that
 * view's visible entries */
void sub_005d6d20(CPU *c){
#ifdef __wasm__
    uint32_t list = c->ecx, view = MR32(c->esp + 4u), fn = MR32(c->esp + 8u);
    sub_005d6d20_orig(c);
    if (view != VIEW9 || fn != LIT_CB || !hl_on() || !MR32(HL_ENABLE)) return;
    make_lights();
    if (g_n) mark(list);
#else
    sub_005d6d20_orig(c);
#endif
}
