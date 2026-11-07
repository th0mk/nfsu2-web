/* plates.c -- license plates with the player's own text.
 *
 * The player sets up to 8 capitals and digits (Settings, Online: settings.ini
 * license_plate, NFSU2_LICENSE_PLATE to the game at Play); none, the game's
 * own plate, is the default, and with none this does nothing at all.  Our cars wear it: player
 * 1's, every local player's in a split race, co-op's player 2's.  Other
 * players in free roam see it: roam_core.c sends it in the HELLO (core 17),
 * and roam_game.c hands each friend's car and its plate here
 * (plates_friend).  Not in a race over the network (Online, LAN, free roam's
 * races between players): those cars are the game's racers, whose plates no
 * one here knows (an Online room has no free roam, an older game sends
 * none), so every car there wears the game's own plate, ours too, and every
 * player sees the same.  In the front end only our car, on the screens that
 * show our car (the garage, the shops): never a car lot's or a car select's.
 *
 * The game's plate is one texture, LICENSE_PLATE (GLOBALB.BUN, 128x64
 * DXT1), named in each car model's texture table (the solid's: +0x19 its
 * entries, +0x2C the table, 8 bytes each: the texture's name hash, then its
 * TextureInfo), and every car of a model shares the model.  So while a car
 * with a plate of its own is drawn (0x620AE0 and 0x6162E0, showoff.c and
 * photo.c: plates_begin/plates_end), each of its models going into the
 * render lists (0x5C5930, overrides.c: plates_model) has its LICENSE_PLATE
 * entry pointed at that plate's TextureInfo, and back right after: the
 * render lists keep the TextureInfo they were handed, so the next car of the
 * model draws with its own.  No draw more, no state more.
 *
 * A plate's texture is a TextureInfo and platform record copied from the
 * game's LICENSE_PLATE (as roam_game.c's paint slots are), with pixels of
 * its own (plate_image.c, A8R8G8B8, from our text and the game's plate) and
 * the game's D3D texture made from them (0x5CE170).  PLATE_SLOTS of them at
 * most, each its text's: as many as the cars that can show one (friends' 64
 * and our own text).  A text no car wants any more leaves its slot to the
 * next text (new pixels into the same texture), and a slot no car wanted for
 * FREE_S goes altogether (the texture released, the memory freed).  At most
 * UPLOADS_PER_FRAME made or remade a frame.  A texture is 43.5 KB in the
 * game's memory and as much on the graphics card: 2.9 MB of each with every
 * slot taken, none while no car has a plate.  So a player who changes plates
 * again and again costs everyone a texture's pixels each time, never more
 * memory, and roam_game.c takes a friend's new plate at most every 10 s
 * (PLATE_EVERY_US), as the server lets them through.
 *
 * Tests: tests/plates_unit.c (the pictures, the HELLO), tests/scripts/
 * license_plate*.txt; "[plates]" log lines. */
#include "cpu.h"
#include "coop.h"
#include "names.h"
#include "plate_image.h"
#include "roam_core.h"
#include "splitscreen_mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

long autotest_frame(void);
const char *autotest_screen_batch(int i);       /* autotest.c: the screens the front end started with its newest */
void metal_upload_texture(unsigned id, unsigned level, const void *data, unsigned pitch, unsigned w, unsigned h, unsigned fmt);

#define GPTR(a)         (g_mem + (uint32_t)(a))   /* guest memory as a host pointer */
#define G_FLOW          0x008654a4u   /* 3 the front end, 6 in the world */
#define G_GAMEMODE      0x00850078u   /* 4: a split race (Quick Race's split screen) */
#define G_NET_ONLINE    0x0089cf69u   /* byte: an online session */
#define G_NET_CLIENT    0x008669f4u   /* > 0 in a network race */
#define G_NET_HOST      0x00866a14u
#define HOOK_RET        0x00581475u   /* the main-loop hook's return address, as guest calls see it */
#define FN_NEW          0x00575620u   /* cdecl (size): operator new */
#define FN_FREE         0x00575630u   /* cdecl (p): operator delete */
#define FN_TEXINFO      0x004901d0u   /* cdecl (hash, default, 0) -> the TextureInfo loaded of that name */
#define FN_TEX_CREATE   0x005ce170u   /* thiscall TextureInfo: its D3D texture from +0x70's pixels */
#define PLATE_HASH      0x029a22f7u   /* bStringHash("LICENSE_PLATE") */
#define PL_CAR          4u            /* Player: its car */
#define RI_CAR          0u            /* a car's render info: its car */
#define TI_SZ           0x7cu         /* TextureInfo: +0 its platform record, +8 its name (24), +0x24 its name's hash, */
#define TI_NAME         0x08u         /* +0x38 its pixels' bytes, +0x3C its palette's, +0x40 level 0's bytes, +0x44 */
#define TI_HASH         0x24u         /* the width, +0x46 the height, +0x48 and +0x49 their log2, +0x4A the kind */
#define TI_BYTES        0x38u         /* (0x22 DXT1, 0x20 A8R8G8B8), +0x4E the levels, +0x6C itself, +0x70 the */
#define TI_PAL          0x3cu         /* pixels, +0x74 the palette */
#define TI_LEVEL0       0x40u
#define TI_W            0x44u
#define TI_H            0x46u
#define TI_LOG_W        0x48u
#define TI_LOG_H        0x49u
#define TI_KIND         0x4au
#define TI_LEVELS       0x4eu
#define TI_SELF         0x6cu
#define TI_PIXELS       0x70u
#define TI_PALETTE      0x74u
#define KIND_ARGB       0x20u
#define PLAT_SZ         0x20u         /* platform record: +0/+4 the links of the game's list of D3D textures, +0x14 the */
#define PLAT_FORMAT     0x14u         /* format (a FourCC, or D3DFMT), +0x18 the D3D texture, +0x1C the render list */
#define PLAT_TEX        0x18u         /* drawing it this frame */
#define PLAT_LIST       0x1cu
#define FMT_DXT1        0x31545844u   /* "DXT1" */
#define FMT_ARGB        21u           /* D3DFMT_A8R8G8B8 */
#define COM_TID         28u           /* the port's D3D texture object (com.c): its texture's id */
#define SOLID_COUNT     0x19u         /* a model's solid: its textures */
#define SOLID_TABLE     0x2cu         /* ...the table of them (hash, TextureInfo) */
#define PLATE_SLOTS     66            /* friends' cars (roam_game.c NSLOT) and our own text, and one to change to */
#define FRIENDS         64            /* roam_game.c's slots */
#define UPLOADS_PER_FRAME 2
#define REUSE_FRAMES    3             /* a slot no car wanted for this many frames may take another text */
#define FREE_S          30.0          /* ...and goes for good after this long */
#define RETRY_FRAMES    60            /* a texture the game would not make: tried again after */

typedef struct {
    char     text[RC_PLATE_MAX + 1];  /* "" none */
    uint32_t ti;                      /* our TextureInfo (0: none), its platform record and pixels after it */
    int      ready;                   /* its pixels are text's, its texture made */
    long     wanted;                  /* the last frame a car wanted it */
    double   wanted_s;
    long     retry;                   /* not before this frame */
} Slot;
static Slot g_slot[PLATE_SLOTS];

/* our text, the friends' cars and theirs */
static char     g_mine[RC_PLATE_MAX + 1];
static int      g_mine_read;
static struct { uint32_t car; char text[RC_PLATE_MAX + 1]; int slot; } g_friend[FRIENDS];
static int      g_mine_slot = -1;

static long     g_frame;              /* plates_frame's count */
static int      g_fe_ours;            /* the front end shows our car (fe_follow) */
static uint32_t g_drawing;            /* the TextureInfo of the car being drawn (0: the game's plate) */
static uint8_t  g_stock[PLATE_W * PLATE_H * 4];
static uint32_t g_stock_px;           /* the game's plate's pixels g_stock was decoded from (0: none) */
static uint8_t  g_px[PLATE_BYTES];    /* a picture being made */
static unsigned g_made, g_freed;

static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n); }
static uint32_t rd32(uint32_t a){ return ok(a, 3) ? MR32(a) : 0u; }
static double now_s(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
/* a guest function with ecx and stack arguments, the CPU as it was after (from the main-loop hook only) */
static uint32_t gcall(CPU *c, uint32_t fn, uint32_t ecx, int n, const uint32_t *arg){
    CPU save = *c;
    c->esp = (c->esp - 256u) & ~15u;
    for (int i = n - 1; i >= 0; i--) PUSH32(arg[i]);
    c->ecx = ecx;
    PUSH32(HOOK_RET);
    dispatch_indirect(c, fn);
    uint32_t r = c->eax;
    *c = save;
    return r;
}

/* ---- our text ------------------------------------------------------------------------------------------------------ */
/* text if it is a plate's (n capitals and digits) and reads as no slur, else none */
static void take(char *dst, const char *text, const char *from){
    size_t n = text ? strlen(text) : 0;
    if (n > RC_PLATE_MAX || !rc_plate_ok(text, (uint32_t)n)){ fprintf(stderr, "[plates] %s: not a plate (capitals and digits, at most %d): none\n", from, RC_PLATE_MAX); n = 0; }
    else if (n && name_blocked(text)){ fprintf(stderr, "[plates] %s reads as a slur: none\n", from); n = 0; }
    if (n) memcpy(dst, text, n);
    dst[n] = 0;
}
/* our plate's text, "" the game's own: NFSU2_LICENSE_PLATE, as the game started */
const char *plates_mine(void){
    if (!g_mine_read){
        g_mine_read = 1;
        take(g_mine, getenv("NFSU2_LICENSE_PLATE"), "NFSU2_LICENSE_PLATE");
        if (g_mine[0]) fprintf(stderr, "[plates] ours: %s\n", g_mine);
    }
    return g_mine;
}

/* roam_game.c, each frame: friend k's car (0: none shown) and its player's plate ("" the game's own) */
void plates_friend(int k, uint32_t car, const char *text){
    if (k < 0 || k >= FRIENDS) return;
    g_friend[k].car = car && text && text[0] ? car : 0;
    snprintf(g_friend[k].text, sizeof g_friend[k].text, "%s", g_friend[k].car ? text : "");
}
/* roam_game.c: the world went, every friend's car with it */
void plates_forget(void){
    for (int k = 0; k < FRIENDS; k++){ g_friend[k].car = 0; g_friend[k].text[0] = 0; g_friend[k].slot = -1; }
}

/* ---- which car wears which ----------------------------------------------------------------------------------------- */
static int network_race(void){
    return MR8(G_NET_ONLINE) || (int32_t)MR32(G_NET_CLIENT) > 0 || (int32_t)MR32(G_NET_HOST) > 0;
}
/* one of our cars: player 1's, a split race's other players', co-op's player 2's */
static int ours(uint32_t car){
    uint32_t p1 = rd32(SPLIT_PLAYERS_INDEX);
    if (car == rd32(p1 + PL_CAR)) return 1;
    if (car == coop_p2_car()) return 1;
    if (MR32(G_GAMEMODE) == 4u || coop_racing_together()){
        uint32_t n = MR32(SPLIT_PLAYERS_COUNT);
        for (uint32_t i = 1; i < n && i < SPLIT_PORT_CAPACITY; i++) if (car == rd32(rd32(SPLIT_PLAYERS_INDEX + 4u * i) + PL_CAR)) return 1;
    }
    return 0;
}
/* The front end draws one car, the one its screen is about: ours in the garage and the shops, the car lot's, a car
 * select's or another player's (Online) elsewhere.  It wears our plate from a screen of ours on, until a screen of
 * others' (any other screen: a dialog, the help bar, changes nothing) */
static int screen_in(const char *s, const char *const *list){
    for (int i = 0; list[i]; i++) if (!strncasecmp(s, list[i], strlen(list[i]))) return 1;
    return 0;
}
static void fe_follow(void){
    static const char *const our_screens[] = { "UI_CareerCrib", "UI_CareerCarSelect", "UI_Choose", "UI_CHoose",
        "UI_PC_Customize", "UI_Paint", "UI_Decal", "UI_Custom", "UI_Rims", "UI_GenericParts", "UI_ICE",
        "UI_Performance", "UI_BuyPerformance", "UI_ShowCase", "UI_Showcase", "UI_Magazine", 0 };
    static const char *const other_screens[] = { "UI_CareerCarLot", "CarLot", "UI_OLCarLot", "UI_QR", "UI_OL",
        "UI_StartCareer", "UI_Main", "MC_", "UI_Options", "UI_PC_LAN", "PC_OL", 0 };
    static uint32_t flow;
    static char told[64];
    if (MR32(G_FLOW) != flow){ flow = MR32(G_FLOW); g_fe_ours = 0; told[0] = 0; }
    if (flow != 3u) return;
    /* the newest screen of either list (the game starts a screen with the garage's backdrop and its help line in
     * the same frame: autotest_screen_batch has them all, the screen first) */
    const char *s = 0, *b;
    for (int i = 0; (b = autotest_screen_batch(i)); i++) if (screen_in(b, other_screens) || screen_in(b, our_screens)) s = b;
    if (!s) return;
    g_fe_ours = screen_in(s, our_screens) && !screen_in(s, other_screens);
    if (g_mine[0] && strcmp(told, s)){
        snprintf(told, sizeof told, "%s", s);
        fprintf(stderr, "[plates] f=%ld %s: %s\n", autotest_frame(), s, g_fe_ours ? "our car, our plate" : "not our car, the game's plate");
    }
}

/* ---- the textures --------------------------------------------------------------------------------------------------- */
static int find(const char *text){
    for (int i = 0; i < PLATE_SLOTS; i++) if (g_slot[i].ti && g_slot[i].text[0] && !strcmp(g_slot[i].text, text)) return i;
    return -1;
}
/* the game's plate decoded (our plates are drawn on it), once its pixels are where we read them last; 0 where they are
 * not as the game's data has them (a plain plate stands in) */
static const uint8_t *stock(uint32_t src){
    uint32_t px = rd32(src + TI_PIXELS), plat = rd32(src);
    if (!px || !ok(px, PLATE_W * PLATE_H / 2u - 1u) || MR16(src + TI_W) != PLATE_W || MR16(src + TI_H) != PLATE_H
        || !ok(plat, PLAT_SZ) || MR32(plat + PLAT_FORMAT) != FMT_DXT1) return 0;
    if (px != g_stock_px){ plate_dxt1(GPTR(px), PLATE_W, PLATE_H, g_stock); g_stock_px = px; }
    return g_stock;
}
/* slot i's texture made, or remade, for text: 1 done */
static int make(CPU *c, int i, const char *text){
    Slot *s = &g_slot[i];
    uint32_t a[3] = { PLATE_HASH, 0, 0 }, src = gcall(c, FN_TEXINFO, 0, 3, a);
    if (!ok(src, TI_SZ) || !ok(rd32(src), PLAT_SZ)) return 0;           /* not loaded (yet) */
    if (!plate_image(text, strlen(text), stock(src), g_px)) return 0;
    uint32_t ti = s->ti;
    if (!ti){
        /* our TextureInfo, its platform record and its pixels in one block of the game's */
        uint32_t size = TI_SZ + PLAT_SZ + 64u + PLATE_BYTES;
        if (!(ti = gcall(c, FN_NEW, 0, 1, &size)) || !ok(ti, size)) return 0;
        uint32_t pl = ti + TI_SZ, pix = (pl + PLAT_SZ + 63u) & ~63u;
        memcpy(GPTR(ti), GPTR(src), TI_SZ); memcpy(GPTR(pl), GPTR(rd32(src)), PLAT_SZ);
        MR32(pl) = MR32(pl + 4u) = 0;                                   /* on no list of the game's */
        MR32(pl + PLAT_TEX) = MR32(pl + PLAT_LIST) = 0;
        MR32(pl + PLAT_FORMAT) = FMT_ARGB;
        char nm[24]; snprintf(nm, sizeof nm, "PLATE_%02d", i);
        for (uint32_t q = 0; q < 24u; q++) MR8(ti + TI_NAME + q) = q < strlen(nm) ? (uint8_t)nm[q] : 0;
        MR32(ti) = pl; MR32(ti + TI_SELF) = ti; MR32(ti + TI_PIXELS) = pix; MR32(ti + TI_PALETTE) = 0;
        MR32(ti + TI_HASH) = 0x504c0000u + (uint32_t)i;                 /* "PL" and the slot: no name the game has */
        MR32(ti + TI_BYTES) = PLATE_BYTES; MR32(ti + TI_PAL) = 0; MR32(ti + TI_LEVEL0) = PLATE_W * PLATE_H * 4;
        MR16(ti + TI_W) = PLATE_W; MR16(ti + TI_H) = PLATE_H;               /* the game's own size: log2 7 and 6 */
        MR8(ti + TI_LOG_W) = 7; MR8(ti + TI_LOG_H) = 6;
        MR8(ti + TI_KIND) = KIND_ARGB; MR8(ti + TI_LEVELS) = PLATE_MIPS;
        s->ti = ti;
    }
    uint32_t pl = MR32(ti), pix = MR32(ti + TI_PIXELS), tex = MR32(pl + PLAT_TEX);
    memcpy(GPTR(pix), g_px, PLATE_BYTES);
    if (!tex) gcall(c, FN_TEX_CREATE, ti, 0, 0);
    else {
        /* the texture is there: the new pixels into it, level by level (the port's texture, com.c) */
        uint32_t off = 0, w = PLATE_W, h = PLATE_H;
        for (int m = 0; m < PLATE_MIPS; m++, w /= 2, h /= 2){
            metal_upload_texture(MR32(tex + COM_TID), (unsigned)m, g_px + off, w * 4u, PLATE_W, PLATE_H, FMT_ARGB);
            off += w * h * 4u;
        }
    }
    if (!MR32(pl + PLAT_TEX)) return 0;
    snprintf(s->text, sizeof s->text, "%s", text);
    s->ready = 1; g_made++;
    return 1;
}
/* slot i gone: its platform record off the game's list of textures (0x5CE170 put it there: +0 the next, +4 the one
 * before, the list's head at 0x872B4C), its texture released (the D3D object's Release until none holds it), and its
 * block freed */
static void drop(CPU *c, int i){
    Slot *s = &g_slot[i];
    uint32_t pl = rd32(s->ti), tex = pl ? rd32(pl + PLAT_TEX) : 0;
    if (pl && ok(rd32(pl), 7) && ok(rd32(pl + 4u), 7)){
        uint32_t next = MR32(pl), before = MR32(pl + 4u);
        MR32(before) = next; MR32(next + 4u) = before;
        MR32(pl) = MR32(pl + 4u) = 0;
    }
    for (int n = 0; tex && n < 8; n++){
        uint32_t release = rd32(rd32(tex) + 8u);
        if (!release || !gcall(c, release, 0, 1, &tex)) break;
    }
    if (pl) MR32(pl + PLAT_TEX) = 0;
    fprintf(stderr, "[plates] f=%ld slot %d (%s) freed: no car wanted it for %.0f s\n", autotest_frame(), i, s->text[0] ? s->text : "none", FREE_S);
    if (s->ti) gcall(c, FN_FREE, 0, 1, &s->ti);
    memset(s, 0, sizeof *s);
    g_freed++;
}
/* text's slot this frame, made now if there is room for it (-1: not yet) */
static int want(CPU *c, const char *text, int *uploads){
    int i = find(text);
    /* (its D3D texture still there: the game releases every one of its list when its device is lost, 0x5CE400) */
    if (i >= 0 && g_slot[i].ready && rd32(rd32(g_slot[i].ti) + PLAT_TEX)){ g_slot[i].wanted = g_frame; g_slot[i].wanted_s = now_s(); return i; }
    if (*uploads >= UPLOADS_PER_FRAME) return -1;
    if (i < 0){
        /* a slot: an empty one first, else the one no car wanted for longest (and not for REUSE_FRAMES) */
        for (int k = 0; k < PLATE_SLOTS; k++){
            Slot *s = &g_slot[k];
            if (g_frame < s->retry || (s->ti && g_frame - s->wanted < REUSE_FRAMES)) continue;
            if (i < 0 || (!s->ti && g_slot[i].ti) || (!!s->ti == !!g_slot[i].ti && s->wanted < g_slot[i].wanted)) i = k;
        }
        if (i < 0) return -1;
    }
    (*uploads)++;
    g_slot[i].ready = 0; g_slot[i].text[0] = 0;
    if (!make(c, i, text)){ g_slot[i].retry = g_frame + RETRY_FRAMES; return -1; }
    g_slot[i].wanted = g_frame; g_slot[i].wanted_s = now_s(); g_slot[i].retry = 0;
    fprintf(stderr, "[plates] f=%ld slot %d: %s\n", autotest_frame(), i, text);
    return i;
}

/* each frame, from the main-loop hook: the textures the cars want */
void plates_frame(CPU *c){
    g_frame++;
    fe_follow();
    const char *mine = plates_mine();
    int uploads = 0, flow = (int)MR32(G_FLOW);
    int net = network_race();
    static int net_was;
    if (net != net_was){
        net_was = net;
        fprintf(stderr, "[plates] f=%ld %s\n", autotest_frame(), net ? "a race over the network: every car the game's own plate" : "no race over the network: plates again");
    }
    g_mine_slot = mine[0] && !net && ((flow == 6) || (flow == 3 && g_fe_ours)) ? want(c, mine, &uploads) : -1;
    for (int k = 0; k < FRIENDS; k++)
        g_friend[k].slot = g_friend[k].car && !net && flow == 6 ? want(c, g_friend[k].text, &uploads) : -1;
    double t = now_s();
    for (int i = 0; i < PLATE_SLOTS; i++)
        if (g_slot[i].ti && g_frame - g_slot[i].wanted > REUSE_FRAMES && t - g_slot[i].wanted_s > FREE_S) drop(c, i);
    static unsigned shown;
    if (g_made + g_freed != shown && (g_made + g_freed) % 16u == 0u){
        int used = 0; for (int i = 0; i < PLATE_SLOTS; i++) used += g_slot[i].ti != 0;
        fprintf(stderr, "[plates] f=%ld %u made, %u freed, %d of %d slots hold one\n", autotest_frame(), g_made, g_freed, used, PLATE_SLOTS);
        shown = g_made + g_freed;
    }
}

/* ---- the drawing ---------------------------------------------------------------------------------------------------- */
/* 0x620AE0 / 0x6162E0 (showoff.c, photo.c): the car of render info ri is drawn now; what was drawing before, back after */
uint32_t plates_begin(uint32_t ri){
    uint32_t before = g_drawing, car = ok(ri, 3) ? MR32(ri + RI_CAR) : 0;
    g_drawing = 0;
    if (!car) return before;
    int slot = -1;
    if (MR32(G_FLOW) == 3u) slot = g_mine_slot;                         /* the front end's one car (plates_frame said whose) */
    else if (ours(car)) slot = g_mine_slot;
    else for (int k = 0; k < FRIENDS; k++) if (g_friend[k].car == car){ slot = g_friend[k].slot; break; }
    if (slot >= 0 && g_slot[slot].ready) g_drawing = g_slot[slot].ti;
    return before;
}
void plates_end(uint32_t before){ g_drawing = before; }
/* 0x5C5930 (overrides.c): a model of the car being drawn into the render lists (item: its model); keep says what to
 * put back after (keep[0] the place, 0: nothing) */
void plates_model(uint32_t item, uint32_t keep[2]){
    keep[0] = 0;
    if (!g_drawing || !ok(item, 0x10u)) return;
    uint32_t solid = MR32(item + 0xcu);
    if (!ok(solid, SOLID_TABLE + 3u)) return;
    uint32_t n = MR8(solid + SOLID_COUNT), table = MR32(solid + SOLID_TABLE);
    if (!n || !ok(table, n * 8u - 1u)) return;
    for (uint32_t k = 0; k < n; k++){
        if (MR32(table + 8u * k) != PLATE_HASH) continue;
        keep[0] = table + 8u * k + 4u; keep[1] = MR32(keep[0]);
        MR32(keep[0]) = g_drawing;
        return;
    }
}
