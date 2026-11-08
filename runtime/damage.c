/* damage.c -- crash damage on the cars' own meshes: the visual part of FRS Mods'
 * "Crashing Foreign Cars" (Damage.asi, NFSU2 v1.2), ported to the browser build.
 *
 * The work is in damagecore.cpp (C++, as the .asi); this file is its link to the
 * game: the overrides and instruction hooks the lifter knows of (gen.sh greps the
 * runtime's .c files for them), and the guest calls the core makes (the game's
 * functions and its Direct3D device, run through the dispatcher on the CPU of the
 * hook that called the core).  The .asi's own hooks, and what stands in here:
 *
 *   CarRenderInfo::Render 0x620AE0 (detour)      showoff.c's override calls damage_render first: the
 *                                                  core's frame work and the bent wheels, then the game's
 *   CarCollisionBody::ApplyImpulse 0x5A0B00      override: the game's, then the impact (vtable slot
 *     (vtable slot +0x50)                          +0x50 checked, as the .asi checks the slot)
 *   CarDamage::SetDamageToZero 0x637D70 (detour) override: the repair, then the game's
 *   Render's open flags 0x620C2B (detour)        HOOK_SITE before the instruction
 *   Render's draws of the body, exhausts and     HOOK_SITE at each call (Body's matrix; the clone of
 *     plates (calls redirected)                    that tip or plate swapped in) and at the next
 *                                                  instruction (the original put back)
 *   UpdateCameraMovers call 0x581493 (main loop) the main-loop hook (roam.c): the scratches' texture pack, a file
 *                                                  of the game folder's mod packs (dmg_scratches_read below)
 *   (none: not in the .asi)                      overrides.c's car texture lists (0x631E20) name a racer's
 *                                                  engine texture, <CAR>_ENGINE, too (damage_texlist_engine), so
 *                                                  the engine a lost hood shows is not black
 *
 * Only what is seen: dents, folds, loose and lost parts (lying on the road, the
 * doors hitting walls and cars), scratches, broken lights, the game's own window
 * cracks, and bent wheels (drawn only).  Not ported (yet): the engine and suspension
 * damage, the health bar and "totaled", the career's saved damage and repairs, the
 * neon and the sounds; nor the .asi's ProStreet textures (broken glass, cracked lenses: EA's),
 * so the windows crack as the game cracks them and a broken lamp only goes dark.  The game's own repair (SetDamageToZero: a race's start, the
 * garage) takes the damage away.
 * In free roam friends see the player's damage (roam_core's RC_MSG_DENTS, to friends who show it, near, roam_game.c):
 * the player's dents are published (damage_net_*), and a friend's car gets the dents its player sent, within limits
 * that keep any other player's messages from slowing this game down or filling its memory (damagecore.cpp's free roam
 * part).  The car-look record never carries them: its bytes say when a friend's car is to be loaded again.
 *   Off unless NFSU2_DAMAGE=1 (the start page's Crash damage, off by default); NFSU2_DAMAGE_LOG=1 logs every
 *   impact. */
#include "cpu.h"
#include "guest.h"
#include "game.h"
#include "widescreen.h"           /* HOOK_SITE */
#include "guest_span.h"           /* g_gwin_hi: the guest heap's window */
#include <stdio.h>
#include <stdlib.h>

void sub_00637d70_orig(CPU *c);   /* CarDamage::SetDamageToZero */
void sub_005a0b00_orig(CPU *c);   /* CarCollisionBody::ApplyImpulse */
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
uint32_t gheap_alloc(int heap, uint32_t n, int zero);
void gheap_free(int heap, uint32_t p);
uint32_t gheap_va_free(void);

/* damagecore.cpp */
int  dmg_init(void);
void dmg_render_entry(uint32_t cri, uint32_t args);
void dmg_impulse(uint32_t body, uint32_t impulse, uint32_t point);
void dmg_zero(uint32_t car_damage, uint32_t caller);
void dmg_open_flags(uint32_t cri, uint32_t frame);
void dmg_body_draw(uint32_t model, uint32_t l2w);
void dmg_marker_pre(int slot, uint32_t model, uint32_t l2w);
void dmg_marker_post(void);
void dmg_main_loop(void);
uint32_t dmg_net_rev(void);
int  dmg_net_mine(uint8_t *epoch, uint16_t *first, uint8_t *dents, int max);
void dmg_net_car(int k, uint32_t car, uint32_t peer, uint8_t epoch, uint16_t first, int n, const uint8_t *dents);

/* the scratches: Crashing Foreign Cars' own DAMAGE_SCRATCH1..4 (FRS Mods' tools/build_pack.py, the bare metal
 * with the scratches in the alpha, four strengths), DXT5, as a raw texture pack (TPK) the game's LoadChunks takes:
 * MODPACKS/DAMAGE/SCRATCHES.TPK in the game folder (tools/damagepack.py), a mod pack's file (fileio.c), which the
 * browser fetches only when it is opened, here, with the damage on.  Read into a block of the guest heap, its data
 * 128-aligned as the pack's textures are (damagecore.cpp gives it to LoadChunks once: dmg_main_loop); the block, or 0
 * without the file (the dents then come without scratches) */
int fileio_mod_pack(const char *rel, char *out, size_t outsz);
#define SCRATCHES_REL "DAMAGE/SCRATCHES.TPK"
#define TPK_CHUNK     0xb3300000u
/* the chunk at b inside [b, end): its size, or -1 */
static int64_t dmg_chunk(uint32_t b, uint32_t end){
    return b + 8u <= end && MR32(b + 4u) <= end - b - 8u ? (int64_t)MR32(b + 4u) : -1;
}
/* the bytes a texture's levels take as the game makes and uploads them (TextureInfo +0x44 width, +0x46 height,
 * +0x4E levels; the platform info's +0x14 its format), as tools/packcheck.py counts them; 0 for a format it does not
 * know */
static uint64_t dmg_tex_bytes(uint32_t w, uint32_t h, uint32_t levels, uint32_t fmt){
    uint32_t blk = 1, bpb;
    switch (fmt){
    case 21: case 22: bpb = 4; break;                   /* A8R8G8B8, X8R8G8B8 */
    case 28: case 41: bpb = 1; break;                   /* A8, P8 */
    case 0x31545844u: blk = 4; bpb = 8; break;          /* DXT1 */
    case 0x33545844u: case 0x35545844u: blk = 4; bpb = 16; break;   /* DXT3, DXT5 */
    default: return 0;
    }
    uint64_t n = 0;
    for (uint32_t lv = 0; lv < (levels ? levels : 1u) && lv < 16u; lv++){
        uint32_t lw = w >> lv ? w >> lv : 1u, lh = h >> lv ? h >> lv : 1u;
        n += (uint64_t)((lw + blk - 1u) / blk) * ((lh + blk - 1u) / blk) * bpb;
    }
    return n;
}
/* the TPK at d (n bytes) as the game's loader trusts it (tools/packcheck.py checks it before it is written): every
 * chunk inside the one that holds it, a hash list (0x33310002), TextureInfos (0x33310004, 0x7C each) and platform
 * infos (0x33310005, 0x20 each) of one count, each texture's hash the list's, its image as big as its size, format
 * and levels take, and it and its palette inside the pixels (0x33320002, from its first 0x80 boundary) */
static int dmg_tpk_ok(uint32_t d, uint32_t n){
    static const uint32_t want[3] = { 0x33310002u, 0x33310004u, 0x33310005u };
    uint32_t at[3] = { 0 }, sz[3] = { 0 }, pix = 0, pend = 0, end = d + n;
    for (uint32_t c = d + 8u; c < end;){
        int64_t s = dmg_chunk(c, end);
        if (s < 0) return 0;
        uint32_t ce = c + 8u + (uint32_t)s;
        if (MR32(c) == 0xb3310000u || MR32(c) == 0xb3320000u)
            for (uint32_t k = c + 8u; k < ce;){
                int64_t ks = dmg_chunk(k, ce);
                if (ks < 0) return 0;
                for (int j = 0; j < 3; j++) if (MR32(k) == want[j]){ at[j] = k + 8u; sz[j] = (uint32_t)ks; }
                if (MR32(k) == 0x33320002u){ pix = (k + 8u + 0x7fu) & ~0x7fu; pend = k + 8u + (uint32_t)ks; }
                k += 8u + (uint32_t)ks;
            }
        c = ce;
    }
    uint32_t cnt = sz[0] / 8u;
    if (!at[0] || !at[1] || !at[2] || !pix || pix > pend || sz[0] != cnt * 8u || sz[1] != cnt * 0x7cu || sz[2] != cnt * 0x20u)
        return 0;
    for (uint32_t k = 0; k < cnt; k++){
        uint32_t ti = at[1] + k * 0x7cu, img = MR32(ti + 0x30u), pal = MR32(ti + 0x34u), isz = MR32(ti + 0x38u), psz = MR32(ti + 0x3cu);
        uint64_t need = dmg_tex_bytes(MR16(ti + 0x44u), MR16(ti + 0x46u), MR8(ti + 0x4eu), MR32(at[2] + k * 0x20u + 0x14u));
        if (MR32(ti + 0x24u) != MR32(at[0] + 8u * k) || !MR16(ti + 0x44u) || !MR16(ti + 0x46u) || !need || isz < need ||
            img > pend - pix || isz > pend - pix - img ||
            (psz && (pal > pend - pix || psz > pend - pix - pal))) return 0;
    }
    return 1;
}
uint32_t dmg_scratches_read(uint32_t *data, uint32_t *size){
    char path[1200];
    *data = 0; *size = 0;
    if (!fileio_mod_pack(SCRATCHES_REL, path, sizeof path)) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    long n = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    uint32_t block = n >= 8 && n <= (16l << 20) && fseek(f, 0, SEEK_SET) == 0 ? gheap_alloc(1, (uint32_t)n + 128u, 0) : 0;
    uint32_t d = (block + 127u) & ~127u;
    if (!block || fread(g_mem + d, 1, (size_t)n, f) != (size_t)n || MR32(d) != TPK_CHUNK || MR32(d + 4u) + 8u != (uint32_t)n ||
        !dmg_tpk_ok(d, (uint32_t)n)){
        fclose(f);
        if (block) gheap_free(1, block);
        fprintf(stderr, "[damage] MODPACKS/" SCRATCHES_REL ": not a texture pack as tools/damagepack.py writes it\n");
        return 0;
    }
    fclose(f);
    *data = d; *size = (uint32_t)n;
    return block;
}

#define APPLY_IMPULSE   0x005a0b00u
#define VT_APPLY_IMPULSE 0x50u        /* its vtable slot */

static CPU *g_c;                      /* the CPU of the hook the core runs under */
static int g_on = -1;

static int on(void){
    if (g_on < 0){
        const char *e = getenv("NFSU2_DAMAGE");
        g_on = e && *e && *e != '0' && dmg_init();   /* off unless asked for */
    }
    return g_on;
}

/* ---- for the core --------------------------------------------------------- */
uint32_t dmg_call(uint32_t fn, int n, const uint32_t *a){ return call_guest(g_c, fn, n, a); }
uint32_t dmg_thiscall(uint32_t fn, uint32_t self, int n, const uint32_t *a){
    uint32_t ecx = g_c->ecx;
    g_c->ecx = self;
    uint32_t r = call_guest(g_c, fn, n, a);
    g_c->ecx = ecx;
    return r;
}
/* a thiscall that returns a float in st0 (the topology's GetElevation): read before the CPU is put back */
void dispatch_indirect(CPU *c, uint32_t addr);
float dmg_thiscall_f(uint32_t fn, uint32_t self, int n, const uint32_t *a){
    CPU *c = g_c;
    CPU save = *c;
    c->ecx = self;
    for (int i = n - 1; i >= 0; i--){ c->esp -= 4u; MR32(c->esp) = a[i]; }
    c->esp -= 4u; MR32(c->esp) = 0xDEADBEEFu;   /* fake return address */
    dispatch_indirect(c, fn);
    const double r = c->st[c->top & 7];
    *c = save;
    return (float)r;
}
uint32_t dmg_alloc(uint32_t n){ return gheap_alloc(1, n, 1); }
void dmg_free(uint32_t p){ gheap_free(1, p); }
int dmg_committed(uint32_t a, uint32_t n){ return guest_ok_bytes(a, n); }
uint32_t dmg_va_free(void){ return gheap_va_free(); }
/* the guest heap has its window above the runtime (guest_span.h: a computer's browser; not phones, tablets, Safari) */
int dmg_big_memory(void){ return g_gwin_hi != 0; }

/* ---- overrides ------------------------------------------------------------ */
/* CarRenderInfo::Render (0x620AE0), before the game's: showoff.c overrides it and calls this first */
void damage_render(CPU *c){
    if (on()){ g_c = c; dmg_render_entry(c->ecx, c->esp + 4u); }   /* its stack arguments: the bent wheels' matrices */
}
void sub_005a0b00(CPU *c){
    uint32_t body = c->ecx, impulse = MR32(c->esp + 4u), point = MR32(c->esp + 8u);
    sub_005a0b00_orig(c);
    if (!on() || !dmg_committed(body, 4) || !dmg_committed(MR32(body) + VT_APPLY_IMPULSE, 4) ||
        MR32(MR32(body) + VT_APPLY_IMPULSE) != APPLY_IMPULSE) return;
    g_c = c;
    dmg_impulse(body, impulse, point);
}
void sub_00637d70(CPU *c){
    if (on()){ g_c = c; dmg_zero(c->ecx, MR32(c->esp)); }
    sub_00637d70_orig(c);
}

/* ---- instruction hooks (debug.c guest_hook) -------------------------------- */
int damage_hook(CPU *c, uint32_t ip){
    switch (ip){
    case HOOK_SITE(0x00620c2b):   /* Render, right after the four open flags: esi the CarRenderInfo */
        if (on()){ g_c = c; dmg_open_flags(c->esi, c->esp); }
        return 1;
    case HOOK_SITE(0x00624499):   /* the part loop's draw: thiscall view (model, l2w, ...) */
        if (on()){ g_c = c; dmg_body_draw(MR32(c->esp), MR32(c->esp + 4u)); }
        return 1;
    case HOOK_SITE(0x00624c52):   /* an exhaust tip's draw */
        if (on()){ g_c = c; dmg_marker_pre(16, MR32(c->esp), MR32(c->esp + 4u)); }
        return 1;
    case HOOK_SITE(0x00625a27):   /* a plate's draw */
        if (on()){ g_c = c; dmg_marker_pre(33, MR32(c->esp), MR32(c->esp + 4u)); }
        return 1;
    case HOOK_SITE(0x00624c57):   /* back from those draws */
    case HOOK_SITE(0x00625a2c):
        if (on()){ g_c = c; dmg_marker_post(); }
        return 1;
    }
    return 0;
}

/* ---- free roam (roam_game.c through roam_host.h): friends see the damage ------- */
/* the damage is on: friends' damage is shown (the HELLO asks for it, RC_HF_DENTS) */
int damage_net_on(void){ return on(); }
/* the player's car's damage as published (+1 for each change), and its dents (RC_MSG_DENTS's) */
uint32_t damage_net_rev(void){ return on() ? dmg_net_rev() : 0u; }
int damage_net_mine(uint8_t *epoch, uint16_t *first, uint8_t *dents, int max){
    return on() ? dmg_net_mine(epoch, first, dents, max) : 0;
}
/* slot k's friend's car (0: none shown) and the dents its player's record has: made on it */
void damage_net_car(CPU *c, int k, uint32_t car, uint32_t peer, uint8_t epoch, uint16_t first, int n, const uint8_t *dents){
    if (!on()) return;
    g_c = c;
    dmg_net_car(k, car, peer, epoch, first, n, dents);
}

/* showoff.c: the show-off opens no part the damage has lost (part: 0 the hood, 1 the trunk lid, 2 the left door, 3 the
 * right door) */
int dmg_part_lost(uint32_t cri, int part);
int damage_part_lost(uint32_t ri, int part){ return on() && dmg_part_lost(ri, part); }

/* overrides.c's car texture lists (0x631E20): a racer's engine texture on them, so that the engine a lost hood leaves
 * in sight has it (the game never opens a car's hood out in a race, so it never loads it: drawn black).  Not a traffic
 * car's (CarTypeInfo +0x844, its use: 2 traffic): those have no damage.  Other players' cars in free roam get it from
 * roam_game.c either way. */
#define CT_SIZE_D     0x890u
#define CT_USE_D      0x844u
#define N_TYPES_D     46u
int damage_texlist_engine(uint32_t ride){
    if (!on() || !ride || !mem_is_committed(ride)) return 0;
    uint32_t types = MR32(G_CARTYPES), t = MR32(ride);
    return types && t < N_TYPES_D && mem_is_committed(types + t * CT_SIZE_D + CT_USE_D) &&
           MR32(types + t * CT_SIZE_D + CT_USE_D) != 2u;
}

/* roam.c's main-loop hook, once a frame */
void damage_frame(CPU *c){
    if (!on()) return;
    g_c = c;
    dmg_main_loop();
}
