/* stance.c -- a car's stance (settings.ini stance, the Mods tab's Stance; off by default): its wheels' camber, front
 * and rear, how far they stand out, and its tires' width, as the car is drawn.  Set per car in the garage (Body Shop,
 * Stance, after Rims); only what is seen changes: the car drives as in the original game.  After Archie (AR4I)'s
 * Stance Mod (nfsmods 1687), which does it for every car at once from its .ini; written anew here.
 *
 * The game (v1.2 NA), in the car's drawing 0x620AE0 (thiscall render info, 12 arguments, ret 0x30):
 *   - Front camber: 0x6270A2..0x627102 makes the angle [esp+0xC8] = int(7.0 [0x7A6754] * f * 65536) / 360 (a 16-bit
 *     binary angle), f = [0x7A6750] when that is not 0 (a tweak, 0 in the game), else 0x59A420(ride + 0x10): 0.5 for
 *     each of two suspension parts the car has (ride +0x363, +0x364: Coilovers and Adjustable Camber Plates), so
 *     3.5 or 7 degrees.  Each front wheel's matrix of arguments 5 and 7 (4 x 0x40 each, wheels 0 and 3 on the car's
 *     left) is then M x Rx(a) (0x5BACF0: a rotation about the car's forward axis, the translation kept): wheel 0 by
 *     +a (0x6278AA, 0x628731), wheel 1 by -a (0x6295AF, 0x62A098).  Nothing tilts the rear wheels.  None where
 *     [esp+0x6C] > 1 (0x627090).
 *   - The render info: +0x400 each wheel's sideways place (2 floats, the left's below 0; 0x62717E), +0x420 each
 *     wheel's tire width (m; the tire's model scaled to it, 0x6271D0).
 * Here the three constants are ours for the call, the rear wheels' matrices are turned the same way before it, and
 * the places and widths are ours: each put back after.
 *
 * Whose car: ours (each player's car in the world; in the front end the car its screens are about, plates.c), with
 * the values of the car the player drives (the career's, else Quick Race's: garage.c), or a friend's in free roam,
 * as its car record says (roam_game.c: RC_EXT_STANCE), unless the player sees the others' cars as the original game
 * has them (Mods tab, "Mod parts on other cars": modpacks.c).  A car with no stance of its own is drawn as the game
 * draws it.
 *
 * The menu (the game's own screens): the Body Shop's list of parts (UI_GenericParts_Browser.fng, made by 0x545B50
 * for the top category 0) gets an entry after Rims, made as 0x53A620 makes Rims' (an item 0x40 bytes, vtable
 * 0x79BE34, +0xC its code, +0x2C its label; 0x52ED80 puts it in the list).  Its Accept starts the fade Rims' does
 * (0x53A310, its sound 0x51DE30), and at the fade's end (0xE1FDE1D1) the Options screen (UI_Options.fng) comes on its
 * page 4, which the game has no entries for ([0x83AA04]; the list's setup 0x4B8470 leaves it empty at 0x4B85ED):
 * here five sliders (optslider.c), with the garage's camera on the car's front (0x4C2100).  Its Back (the fade's end
 * with +0x138 0x911AB364) goes to the Body Shop again, not to the Options menu. */
#include "cpu.h"
#include "guest.h"
#include "game.h"
#include "setting.h"
#include "optslider.h"
#include "widescreen.h"           /* HOOK_SITE */
#include "featurelist.h"
#include "coop.h"
#include "splitscreen_mem.h"
#include "stance.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

long autotest_frame(void);
uint32_t language_add_text(const char *name, const char *text);
const char *language_name(void);
void language_game_letters(const char *utf8, char *out, size_t n);
void dispatch_patch(uint32_t a, void (*f)(CPU *));
uint32_t garage_find(uint32_t db, uint32_t h);
int modpacks_others_original(void);
int plates_fe_ours(void);
const char *plates_fe_screen(void);
void sub_00566490(CPU *c);                       /* the lifted ones the patches run: the Body Shop list's messages */
void sub_0049c690(CPU *c);                       /* ... the Options screen's */

/* the car's drawing */
#define G_CAMBER_F     0x007a6750u   /* float: the camber's factor where not 0 (the game: 0, the parts decide) */
#define G_CAMBER_DEG   0x007a6754u   /* float: the camber at factor 1, degrees (7.0) */
#define G_CAMBER_K     0x007a6758u   /* float: 0.03, times [0x7A6750] (a shift of the front wheels with the tweak) */
#define CD_ARG_WHEELS  0x14u         /* the drawing's arguments 5 and 7 (esp at its entry: the return address) */
#define CD_ARG_WHEELS2 0x1cu
#define WHEEL_MX       0x40u
#define RI_CAR         0u            /* render info: its car */
#define RI_RIDE        4u            /* ... its ride (+0 its car type's index) */
#define CARTYPES_N     46u           /* G_CARTYPES' car types (vinylcat.c) */
#define CARTYPE_SZ     0x890u
#define CARTYPE_HASH   0xd0u         /* a car type's hash: a car record's car type */
#define RI_WHEEL_Y     0x400u        /* ... each wheel's sideways place, two floats (the car's left: below 0) */
#define RI_TIRE_W      0x420u        /* ... each wheel's tire width, m */
#define CAR_RIDE       0x10u         /* car: its ride */
#define RIDE_COILOVERS 0x363u        /* ride: the two suspension parts the game's camber comes from (0x59A420) */
#define RIDE_PLATES    0x364u
#define PL_CAR         4u            /* Player: its car */
#define ST_REC_TYPE    0x20u         /* a car record: its car type (garage.c R_TYPE) */
#define GAME_CAMBER    7.0f          /* [0x7A6754] */

/* the menu */
#define FN_ITEM_ADD    0x0052ed80u   /* thiscall list (item, 1, -1) -> the item's icon object */
#define FN_GO_FADE     0x0053a310u   /* thiscall parts screen: Rims' Accept: the screen's fade out ([0x839BC0] 1) */
#define FN_FE_SOUND    0x0051de30u   /* cdecl (8, screen +0x30): the menu's sound */
#define FN_SWITCH      0x00537980u   /* cdecl (from package name, to package name, 0): another screen */
#define FN_TEXT        0x00495460u   /* cdecl (package name, object hash, text hash) */
#define FN_CAMERA      0x004c2100u   /* cdecl (view, 0, 0, 1): the garage's camera to a view (0x7F4608's) */
#define FN_CAMERA_BACK 0x004c2140u   /* cdecl (0, 1): its orbit again */
#define FN_OPT_ADD     0x0054f410u   /* the Options list's add (optslider.c) */
#define VT_PART_ITEM   0x0079be34u   /* a Body Shop list item: +0xC its code, +0x2C its label hash */
#define ICON_STANCE    0xb802f10eu   /* the Car Shop's Change Body Parts: the car between two arrows */
#define P_LIST         0x4cu         /* the parts screen: its list */
#define P_SELECTED     0x58u         /* ... the item highlighted */
#define P_COUNT        0x1c4u        /* ... its items */
#define P_BUSY         0x1d4u        /* ... byte: a fade to another screen under way */
#define I_CODE         0x0cu
#define I_LABEL        0x2cu
#define STANCE_CODE    0x1eu         /* our item's code: no category's (parts 1-0x20 but this, rims 0x1D, carbon 0x5D-0x60) */
#define ROW_PARTS_CODE 0x007f867cu   /* the screen table's UI_GenericParts_Browser.fng row +0x10: the code the list is made on */
#define G_RIMS_GO      0x00839bc0u   /* byte: Rims' fade under way (0x53A310): the parts list's fade end goes to the rims */
#define G_OPT_PAGE     0x0083aa04u   /* the Options screen's page (0 Audio .. 3 Player; 4: none of the game's) */
#define PAGE_STANCE    4u
#define O_BACK         0x138u        /* the Options screen: the message its fade out ends with */
#define MSG_ACCEPT     0x406415e3u
#define MSG_ACCEPT2    0x0c407210u
#define MSG_FADE_DONE  0xe1fde1d1u
#define MSG_DEFAULTS   0x11111111u
#define MSG_BACK_DONE  0x911ab364u
#define OBJ_TITLE      0x42adb44cu   /* the Options screen's title */
#define VIEW_FRONT     0xa1u         /* the garage's Hydraulics view: the car's front, low */
#define PKG_OPTIONS    0x0078fa50u   /* "UI_Options.fng" */
#define PKG_PARTS      0x00790634u   /* "UI_GenericParts_Browser.fng" */
#define PKG_RATING     "UI_Menu_Asset_Reputation.fng"   /* the shops' Visual Rating */
#define OBJ_PANEL      0x8d7e0c53u   /* UI_Options.fng: the group of the title and the panel's two backings */
#define OBJ_PANEL_OUT  0x0de769b5u   /* ... the outer backing (alpha 215) */
#define OBJ_PANEL_IN   0x371f1c93u   /* ... the inner one (alpha 200) */
#define PANEL_ALPHA    0x80u         /* theirs on the Stance page: the car seen through it, the labels still read */

/* ---- the values ------------------------------------------------------------------------------------------------- */
typedef struct { uint32_t handle, type; StanceV v; } Entry;
/* each player's cars with a stance of their own, of the profile name (that player's profile then; "" none) */
static struct { Entry e[STANCE_CARS]; int n; char name[33]; } g_cars[2];
static StanceV g_friend[STANCE_FRIENDS]; static uint32_t g_friend_car[STANCE_FRIENDS];

int stance_on(void){
    static int on = -1;
    if (on < 0){ on = setting_bool("NFSU2_STANCE", 0); fprintf(stderr, "[stance] %s\n", on ? "on: Body Shop, Stance" : "off: every car of ours as the game draws it"); }
    return on;
}
int stance_ok(const StanceV *v){
    return v->fc >= STANCE_CAMBER_MIN && v->fc <= STANCE_CAMBER_MAX && v->rc >= STANCE_CAMBER_MIN && v->rc <= STANCE_CAMBER_MAX
        && v->fo >= STANCE_OFFSET_MIN && v->fo <= STANCE_OFFSET_MAX && v->ro >= STANCE_OFFSET_MIN && v->ro <= STANCE_OFFSET_MAX
        && v->tw >= STANCE_TIRE_MIN && v->tw <= STANCE_TIRE_MAX;
}
/* the handle of the car player p drives (garage.c's swap_in) */
static uint32_t driven(int p){
    return MR32(G_GAMEMODE) == 1u ? (p ? 0u : MR32(G_CAREER_CAR)) : MR32(G_QR_CAR + 4u * (uint32_t)p);
}
/* player p's car record of handle h, 0: none */
static uint32_t record(int p, uint32_t h){ return garage_find(G_FECARS + (uint32_t)p * G_FECARS_SZ, h); }
/* player 1's profile now ("" none); player 2's: the one its cars came with */
static const char *profile(int p){
    static char nm[33];
    if (p) return g_cars[1].name;
    nm[0] = 0;
    if (MR8(G_PROFILE_NAME + 0x20u)) guest_read_str(G_PROFILE_NAME, nm, sizeof nm);
    return nm;
}
/* stances set with no profile, then one made of that state (no load since): that profile's */
static void adopt(int p){
    if (p || g_cars[0].name[0] || !profile(0)[0]) return;
    snprintf(g_cars[0].name, sizeof g_cars[0].name, "%s", profile(0));
    if (g_cars[0].n) fprintf(stderr, "[stance] %d car%s' stance set with no profile: now %s's\n", g_cars[0].n, g_cars[0].n == 1 ? "" : "s", g_cars[0].name);
}
static Entry *entry(int p, uint32_t h){
    if (!h || p < 0 || p > 1) return 0;
    adopt(p);
    if (strcasecmp(g_cars[p].name, profile(p))) return 0;           /* another profile's (a new one since) */
    for (int i = 0; i < g_cars[p].n; i++) if (g_cars[p].e[i].handle == h) return &g_cars[p].e[i];
    return 0;
}
/* test aid, script runs only: NFSU2_TEST_STANCE_MINE=<front camber>,<rear>,<front offset>,<rear>,<tires> (stance.h's
 * units): player 1's car has it, whatever car it is (a pair's free roam without the garage) */
static const StanceV *test_mine(void){
    static int read; static StanceV v; static int on;
    if (!read){
        read = 1;
        const char *sc = getenv("NFSU2_INPUT_SCRIPT"), *e = getenv("NFSU2_TEST_STANCE_MINE");
        int a[5];
        if (sc && *sc && e && sscanf(e, "%d,%d,%d,%d,%d", &a[0], &a[1], &a[2], &a[3], &a[4]) == 5){
            v.fc = (int16_t)a[0]; v.rc = (int16_t)a[1]; v.fo = (int16_t)a[2]; v.ro = (int16_t)a[3]; v.tw = (uint16_t)a[4];
            on = stance_ok(&v);
            fprintf(stderr, "[stance] TEST: player 1's car %s\n", on ? "with the test's stance" : "(the test's stance out of range: none)");
        }
    }
    return on ? &v : 0;
}
/* player p's car's stance, 0: none of its own (the stance off: none) */
const StanceV *stance_mine(int p){
    if (!stance_on()) return 0;
    if (!p && test_mine()) return test_mine();
    uint32_t h = driven(p), r = record(p, h);
    Entry *e = entry(p, h);
    return e && r && modparts_car_type(p, r) == e->type ? &e->v : 0;   /* (its car type as set: not a car changed elsewhere) */
}
/* the game's front camber for a ride (degrees), from its two suspension parts */
static float game_camber(uint32_t ride){
    if (!guest_ok(ride, RIDE_PLATES)) return 0;
    return GAME_CAMBER * (0.5f * (MR8(ride + RIDE_COILOVERS) != 0) + 0.5f * (MR8(ride + RIDE_PLATES) != 0));
}
/* a new entry for handle h in player p's list, which stays in the handles' order (one order for "anything to save?"
 * whatever order the stances were set in), 0: full */
static Entry *insert(int p, uint32_t h){
    if (g_cars[p].n == STANCE_CARS) return 0;
    int i = 0;
    while (i < g_cars[p].n && g_cars[p].e[i].handle < h) i++;
    memmove(&g_cars[p].e[i + 1], &g_cars[p].e[i], sizeof g_cars[p].e[0] * (size_t)(g_cars[p].n - i));
    g_cars[p].n++;
    memset(&g_cars[p].e[i], 0, sizeof g_cars[p].e[i]); g_cars[p].e[i].handle = h;
    return &g_cars[p].e[i];
}
static void set_mine(int p, uint32_t h, const StanceV *v){
    uint32_t r = record(p, h);
    if (!r) return;
    if (strcasecmp(g_cars[p].name, profile(p))){                     /* the first of this profile's: the others' go */
        g_cars[p].n = 0; snprintf(g_cars[p].name, sizeof g_cars[p].name, "%s", profile(p));
    }
    Entry *e = entry(p, h);
    if (!v){                                                         /* none: the game's look (the others in their order) */
        if (e){ int i = (int)(e - g_cars[p].e); memmove(e, e + 1, sizeof *e * (size_t)(g_cars[p].n - 1 - i)); g_cars[p].n--; }
        return;
    }
    if (!e && !(e = insert(p, h))) return;
    e->type = modparts_car_type(p, r); e->v = *v;                  /* (a car of a car type the game has not: its own) */
}

/* ---- kept in MODPARTS.DAT (modpacks.c) ---------------------------------------------------------------------------- */
int stance_entries(int p, uint32_t (*out)[8], int max){
    if (p < 0 || p > 1) return 0;
    adopt(p);
    if (strcasecmp(g_cars[p].name, profile(p))){                    /* another profile's: not this one's */
        if (g_cars[p].n) fprintf(stderr, "[stance] %d stance%s of %s's not saved with %s's cars\n", g_cars[p].n, g_cars[p].n == 1 ? "" : "s",
                                 g_cars[p].name[0] ? g_cars[p].name : "no profile", profile(p));
        return 0;
    }
    int n = 0;
    for (int i = 0; i < g_cars[p].n && n < max; i++){
        const Entry *e = &g_cars[p].e[i];
        uint32_t *w = out[n++];
        w[0] = e->handle; w[1] = e->type;
        w[2] = (uint16_t)e->v.fc | (uint32_t)(uint16_t)e->v.rc << 16;
        w[3] = (uint16_t)e->v.fo | (uint32_t)(uint16_t)e->v.ro << 16;
        w[4] = e->v.tw; w[5] = 0; w[6] = 0; w[7] = STANCE_KIND;
    }
    return n;
}
void stance_loaded(int p, const char *name){
    if (p < 0 || p > 1) return;
    g_cars[p].n = 0;
    snprintf(g_cars[p].name, sizeof g_cars[p].name, "%s", name ? name : "");
}
int stance_load(int p, const uint32_t w[8]){
    if (p < 0 || p > 1 || w[7] != STANCE_KIND || !w[0]) return 0;
    StanceV v = { (int16_t)(w[2] & 0xffffu), (int16_t)(w[2] >> 16), (int16_t)(w[3] & 0xffffu), (int16_t)(w[3] >> 16), (uint16_t)w[4] };
    if (w[4] > 0xffffu || !stance_ok(&v)) return 0;                 /* (a later page's values: kept as they are) */
    for (int i = 0; i < g_cars[p].n; i++) if (g_cars[p].e[i].handle == w[0]) return 0;
    Entry *e = insert(p, w[0]);
    if (!e) return 0;
    e->type = w[1]; e->v = v;
    return 1;
}
int stance_has(int p, uint32_t h){
    if (p < 0 || p > 1 || strcasecmp(g_cars[p].name, profile(p))) return 0;
    for (int i = 0; i < g_cars[p].n; i++) if (g_cars[p].e[i].handle == h) return 1;
    return 0;
}
void stance_deleted(const char *name){
    for (int p = 0; p < 2; p++) if (name && !strcasecmp(g_cars[p].name, name)){ g_cars[p].n = 0; g_cars[p].name[0] = 0; }
}
static struct { int on, n; Entry e[STANCE_CARS]; char name[33]; } g_split;
void stance_split_keep(void){
    g_split.on = 1; g_split.n = g_cars[0].n; memcpy(g_split.e, g_cars[0].e, sizeof g_split.e);
    snprintf(g_split.name, sizeof g_split.name, "%s", g_cars[0].name);
}
void stance_split_back(void){
    if (!g_split.on) return;
    g_split.on = 0; g_cars[0].n = g_split.n; memcpy(g_cars[0].e, g_split.e, sizeof g_split.e);
    snprintf(g_cars[0].name, sizeof g_cars[0].name, "%s", g_split.name);
}
void stance_car_gone(int p, uint32_t h){
    if (p < 0 || p > 1 || !h) return;
    for (int i = 0; i < g_cars[p].n; i++)
        if (g_cars[p].e[i].handle == h){
            memmove(&g_cars[p].e[i], &g_cars[p].e[i + 1], sizeof g_cars[p].e[0] * (size_t)(g_cars[p].n - 1 - i)); g_cars[p].n--;
            fprintf(stderr, "[stance] the car %08x has gone: its stance with it\n", h);
            return;
        }
}
void stance_car_moved(int p, uint32_t hf, uint32_t ht){
    if (p < 0 || p > 1 || !hf || hf == ht) return;
    stance_car_gone(p, ht);
    for (int i = 0; i < g_cars[p].n; i++)
        if (g_cars[p].e[i].handle == hf){
            Entry moved = g_cars[p].e[i];
            memmove(&g_cars[p].e[i], &g_cars[p].e[i + 1], sizeof moved * (size_t)(g_cars[p].n - 1 - i)); g_cars[p].n--;
            Entry *e = insert(p, ht);
            if (e){ moved.handle = ht; *e = moved; }
            return;
        }
}

/* ---- friends' (roam_game.c, RC_EXT_STANCE) ------------------------------------------------------------------------ */
static int16_t clamp16(int v, int lo, int hi){ return (int16_t)(v < lo ? lo : v > hi ? hi : v); }
/* the player's car's stance as the others see it: 1 with v (camber front, rear, offset front, rear, tires' width) */
int stance_wire_mine(int16_t v[5]){
    const StanceV *s = stance_mine(0);
    if (!s) return 0;
    v[0] = s->fc; v[1] = s->rc; v[2] = s->fo; v[3] = s->ro; v[4] = (int16_t)s->tw;
    return 1;
}
/* each frame: friend k's car shown (0: none) and its stance (has 0: none of its own), within this game's ranges */
void stance_wire_car(int k, uint32_t car, int has, const int16_t v[5]){
    static StanceV said[STANCE_FRIENDS]; static uint8_t said_on[STANCE_FRIENDS];   /* (the log: each slot's values once) */
    if (k < 0 || k >= STANCE_FRIENDS) return;
    g_friend_car[k] = car && has ? car : 0;
    if (!g_friend_car[k]) return;
    StanceV *f = &g_friend[k];
    f->fc = clamp16(v[0], STANCE_CAMBER_MIN, STANCE_CAMBER_MAX); f->rc = clamp16(v[1], STANCE_CAMBER_MIN, STANCE_CAMBER_MAX);
    f->fo = clamp16(v[2], STANCE_OFFSET_MIN, STANCE_OFFSET_MAX); f->ro = clamp16(v[3], STANCE_OFFSET_MIN, STANCE_OFFSET_MAX);
    f->tw = (uint16_t)clamp16(v[4], STANCE_TIRE_MIN, STANCE_TIRE_MAX);
    if (said_on[k] && !memcmp(&said[k], f, sizeof *f)) return;
    said[k] = *f; said_on[k] = 1;
    fprintf(stderr, "[stance] f=%ld slot %d's car: camber %d/%d, offset %d/%d mm, tires %u %%%s\n", autotest_frame(), k,
            f->fc, f->rc, f->fo, f->ro, f->tw, modpacks_others_original() ? " (not shown: Mods on other cars, Original)" : "");
}
void stance_forget(void){ memset(g_friend_car, 0, sizeof g_friend_car); }

/* ---- the drawing ------------------------------------------------------------------------------------------------- */
static struct {
    int      on;
    uint32_t keep_c[3];
    uint32_t mx[4];                  /* the rear wheels' matrices turned: argument 5's and 7's, wheels 2 and 3 */
    float    keep_mx[4][12];
    uint32_t ri;
    float    keep_y[8], keep_w[4];
} D;
static struct { int open; StanceV v; float game_fc; } M;          /* the menu's car, while its page is open */

/* the car type (its hash, as a car record has it) of the ride of render info ri, 0: none */
static uint32_t ride_type(uint32_t ri){
    uint32_t ride = guest_rd32(ri + RI_RIDE), types = guest_rd32(G_CARTYPES), t = ride ? guest_rd32(ride) : 0xffffffffu;
    return t < CARTYPES_N && guest_ok(types + t * CARTYPE_SZ, CARTYPE_HASH + 3u) ? MR32(types + t * CARTYPE_SZ + CARTYPE_HASH) : 0;
}
/* the stance the car of render info ri is drawn with, 0: the game's */
static const StanceV *of_drawn(uint32_t ri){
    uint32_t car = guest_rd32(ri + RI_CAR), flow = MR32(G_FLOW);
    if (flow == 3u){
        if (M.open) return &M.v;
        /* the car the front end's screens are about: the car the player drives, not one a car select shows */
        if (!car || !plates_fe_ours() || strstr(plates_fe_screen(), "CarSelect")) return 0;
        uint32_t r = record(0, driven(0));
        return r && ride_type(ri) == MR32(r + ST_REC_TYPE) ? stance_mine(0) : 0;      /* (the car shown: the record's car type here) */
    }
    if (flow != 6u || !car) return 0;
    if (car == guest_rd32(guest_rd32(SPLIT_PLAYERS_INDEX) + PL_CAR)) return stance_mine(0);
    if (car == coop_p2_car()) return stance_mine(1);
    uint32_t n = MR32(SPLIT_PLAYERS_COUNT);                             /* split screen's player 2 (as plates.c's ours) */
    if ((MR32(G_GAMEMODE) == 4u || coop_racing_together()) && n > 1u && n <= SPLIT_PORT_CAPACITY &&
        car == guest_rd32(guest_rd32(SPLIT_PLAYERS_INDEX + 4u) + PL_CAR)) return stance_mine(1);
    if (modpacks_others_original()) return 0;
    for (int k = 0; k < STANCE_FRIENDS; k++) if (g_friend_car[k] == car) return &g_friend[k];
    return 0;
}

/* m (row-major 4x4, rows 0..2 its rotation) = m x Rx(a), as 0x5BACF0 turns the front wheels */
static void turn(uint32_t m, float a, float keep[12]){
    float r[12];
    for (int k = 0; k < 12; k++) keep[k] = r[k] = MRF32(m + 4u * (uint32_t)k);
    float cs = cosf(a), sn = sinf(a);
    for (int row = 0; row < 3; row++){
        float y = r[row * 4 + 1], z = r[row * 4 + 2];
        MRF32(m + 4u * (uint32_t)(row * 4 + 1)) = y * cs - z * sn;
        MRF32(m + 4u * (uint32_t)(row * 4 + 2)) = y * sn + z * cs;
    }
}

/* showoff.c's 0x620AE0, before the game's drawing and after it */
void stance_draw_begin(CPU *c){
    D.on = 0;
    uint32_t ri = c->ecx;
    if (!guest_ok(ri, RI_TIRE_W + 0x10u)) return;
    const StanceV *s = of_drawn(ri);
    if (!s) return;
    D.on = 1; D.ri = ri;
    D.keep_c[0] = MR32(G_CAMBER_F); D.keep_c[1] = MR32(G_CAMBER_DEG); D.keep_c[2] = MR32(G_CAMBER_K);
    MRF32(G_CAMBER_F) = 1.0f; MRF32(G_CAMBER_DEG) = s->fc * 0.1f; MRF32(G_CAMBER_K) = 0.0f;
    for (int k = 0; k < 8; k++){
        float y = D.keep_y[k] = MRF32(ri + RI_WHEEL_Y + 4u * (uint32_t)k), o = (k < 4 ? s->fo : s->ro) * 0.001f;
        MRF32(ri + RI_WHEEL_Y + 4u * (uint32_t)k) = y + (y < 0 ? -o : o);
    }
    for (int k = 0; k < 4; k++){ D.keep_w[k] = MRF32(ri + RI_TIRE_W + 4u * (uint32_t)k); MRF32(ri + RI_TIRE_W + 4u * (uint32_t)k) = D.keep_w[k] * s->tw * 0.01f; }
    float a = s->rc * 0.1f * (float)M_PI / 180.0f;
    uint32_t w[2] = { MR32(c->esp + CD_ARG_WHEELS), MR32(c->esp + CD_ARG_WHEELS2) };
    for (int set = 0; set < 2; set++){
        int ok = a != 0 && guest_ok_bytes(w[set], 4u * WHEEL_MX);
        for (int i = 2; i < 4; i++){
            uint32_t m = ok ? w[set] + WHEEL_MX * (uint32_t)i : 0;
            D.mx[set * 2 + (i - 2)] = m;
            if (m) turn(m, i == 2 ? -a : a, D.keep_mx[set * 2 + (i - 2)]);   /* wheel 2 on the right, as wheel 1 */
        }
    }
}
void stance_draw_end(void){
    if (!D.on) return;
    D.on = 0;
    MR32(G_CAMBER_F) = D.keep_c[0]; MR32(G_CAMBER_DEG) = D.keep_c[1]; MR32(G_CAMBER_K) = D.keep_c[2];
    for (int k = 0; k < 8; k++) MRF32(D.ri + RI_WHEEL_Y + 4u * (uint32_t)k) = D.keep_y[k];
    for (int k = 0; k < 4; k++) MRF32(D.ri + RI_TIRE_W + 4u * (uint32_t)k) = D.keep_w[k];
    for (int k = 0; k < 4; k++){
        if (!D.mx[k]) continue;
        for (int j = 0; j < 12; j++) MRF32(D.mx[k] + 4u * (uint32_t)j) = D.keep_mx[k][j];
    }
}

/* ---- the menu ---------------------------------------------------------------------------------------------------- */
/* the texts, in the game's language (English in those whose fonts draw only ASCII besides their own letters: Russian,
 * Ukrainian, Greek, Chinese, Japanese, Korean, Thai), in its fonts' letters (language.c); short, as the Options rows
 * have room for: the value follows each */
enum { T_STANCE, T_FCAMBER, T_RCAMBER, T_FOFFSET, T_ROFFSET, T_TIRES, T_N };
static const char *text(int t){
    static const char *const L[][1 + T_N] = {          /* a language, then its texts */
        { "english", "Stance", "Front Camber", "Rear Camber", "Front Offset", "Rear Offset", "Tire Width" },
        { "french", "Stance", "Carrossage AV", "Carrossage AR", "Voie AV", "Voie AR", "Largeur pneus" },
        { "german", "Stance", "Sturz vorn", "Sturz hinten", "Spurweite vorn", "Spurweite hinten", "Reifenbreite" },
        { "italian", "Stance", "Campanatura ant.", "Campanatura post.", "Carreggiata ant.", "Carreggiata post.", "Larghezza gomme" },
        { "spanish", "Stance", "Ca\xc3\xad" "da del.", "Ca\xc3\xad" "da tras.", "V\xc3\xad" "a del.", "V\xc3\xad" "a tras.", "Ancho neum\xc3\xa1" "tico" },
        { "dutch", "Stance", "Camber voor", "Camber achter", "Spoor voor", "Spoor achter", "Bandbreedte" },
        { "swedish", "Stance", "Camber fram", "Camber bak", "Sp\xc3\xa5" "rvidd fram", "Sp\xc3\xa5" "rvidd bak", "D\xc3\xa4" "ckbredd" },
        { "danish", "Stance", "Camber for", "Camber bag", "Sporvidde for", "Sporvidde bag", "D\xc3\xa6" "kbredde" },
        { "portuguese", "Stance", "Cambagem diant.", "Cambagem tras.", "Bitola diant.", "Bitola tras.", "Largura pneu" },
        { "polish", "Stance", "Pochylenie prz\xc3\xb3" "d", "Pochylenie ty\xc5\x82", "Rozstaw prz\xc3\xb3" "d", "Rozstaw ty\xc5\x82", "Szeroko\xc5\x9b" "\xc4\x87" " opon" },
        { "turkish", "Stance", "\xc3\x96" "n kamber", "Arka kamber", "\xc3\x96" "n ofset", "Arka ofset", "Lastik geni\xc5\x9f" "li\xc4\x9f" "i" },
        { "hungarian", "Stance", "Els\xc5\x91" " d\xc5\x91" "l\xc3\xa9" "s", "H\xc3\xa1" "ts\xc3\xb3" " d\xc5\x91" "l\xc3\xa9" "s", "Els\xc5\x91" " nyomt\xc3\xa1" "v", "H\xc3\xa1" "ts\xc3\xb3" " nyomt\xc3\xa1" "v", "Gumisz\xc3\xa9" "less\xc3\xa9" "g" },
        { "czech", "Stance", "Odklon vp\xc5\x99" "edu", "Odklon vzadu", "Rozchod vp\xc5\x99" "edu", "Rozchod vzadu", "\xc5\xa0" "\xc3\xad" "\xc5\x99" "ka pneu" },
        { "slovak", "Stance", "Odklon vpredu", "Odklon vzadu", "Rozchod vpredu", "Rozchod vzadu", "\xc5\xa0" "\xc3\xad" "rka pneu" },
    };
    static char got[T_N][48]; static int made;
    if (!made){
        made = 1;
        const char *l = language_name();
        unsigned k = 0;
        for (unsigned i = 0; i < sizeof L / sizeof L[0]; i++) if (!strcmp(l, L[i][0])) k = i;
        for (int i = 0; i < T_N; i++) language_game_letters(L[k][1 + i], got[i], sizeof got[i]);
    }
    return got[t];
}
/* the degree sign in the game's fonts, "" where they have none */
static const char *degree(void){
    static char d[4]; static int made;
    if (!made){ made = 1; language_game_letters("\xc2\xb0", d, sizeof d); if (!strcmp(d, "?")) d[0] = 0; }
    return d;
}
static uint32_t g_label;                         /* the Body Shop entry's label */
static uint32_t label_hash(void){
    if (!g_label) g_label = language_add_text("STANCE_MENU", text(T_STANCE));
    return g_label;
}

/* the sliders: each in its own units (degrees, millimetres, per cent) */
static float v_fc(void){ return M.v.fc * 0.1f; }
static float v_rc(void){ return M.v.rc * 0.1f; }
static float v_fo(void){ return (float)M.v.fo; }
static float v_ro(void){ return (float)M.v.ro; }
static float v_tw(void){ return (float)M.v.tw; }
static void moved(const char *what, int16_t *field, int now){
    *field = (int16_t)now;
    fprintf(stderr, "[stance] f=%ld %s %d\n", autotest_frame(), what, now);
}
static void m_fc(float v){ moved("front camber (0.1 deg)", &M.v.fc, (int)lroundf(v * 10)); }
static void m_rc(float v){ moved("rear camber (0.1 deg)", &M.v.rc, (int)lroundf(v * 10)); }
static void m_fo(float v){ moved("front offset (mm)", &M.v.fo, (int)lroundf(v)); }
static void m_ro(float v){ moved("rear offset (mm)", &M.v.ro, (int)lroundf(v)); }
static void m_tw(float v){ M.v.tw = (uint16_t)lroundf(v); fprintf(stderr, "[stance] f=%ld tire width %u %%\n", autotest_frame(), M.v.tw); }
static char g_lbl[64];
/* the decimal point as the game's texts have it in its language (its "0,00" of the Visual Rating) */
static char point(void){
    static const char *const DOT[] = { "english", "chinese", "japanese", "korean", "thai" };
    const char *l = language_name();
    for (unsigned i = 0; i < sizeof DOT / sizeof DOT[0]; i++) if (!strcmp(l, DOT[i])) return '.';
    return ',';
}
static const char *l_camber(int t, int tenths){
    snprintf(g_lbl, sizeof g_lbl, "%s  %d%c%d%s", text(t), tenths / 10, point(), tenths % 10, degree());
    return g_lbl;
}
static const char *l_offset(int t, int mm){ snprintf(g_lbl, sizeof g_lbl, "%s  %+d mm", text(t), mm); return g_lbl; }
static const char *l_fc(void){ return l_camber(T_FCAMBER, M.v.fc); }
static const char *l_rc(void){ return l_camber(T_RCAMBER, M.v.rc); }
static const char *l_fo(void){ return l_offset(T_FOFFSET, M.v.fo); }
static const char *l_ro(void){ return l_offset(T_ROFFSET, M.v.ro); }
static const char *l_tw(void){ snprintf(g_lbl, sizeof g_lbl, "%s  %u%%", text(T_TIRES), M.v.tw); return g_lbl; }
static OptSlider g_sl[5] = {
    { "Front Camber", STANCE_CAMBER_MIN * 0.1f, STANCE_CAMBER_MAX * 0.1f, 0.5f, v_fc, m_fc, l_fc, 0 },
    { "Rear Camber",  STANCE_CAMBER_MIN * 0.1f, STANCE_CAMBER_MAX * 0.1f, 0.5f, v_rc, m_rc, l_rc, 0 },
    { "Front Offset", STANCE_OFFSET_MIN, STANCE_OFFSET_MAX, 5.0f, v_fo, m_fo, l_fo, 0 },
    { "Rear Offset",  STANCE_OFFSET_MIN, STANCE_OFFSET_MAX, 5.0f, v_ro, m_ro, l_ro, 0 },
    { "Tire Width",   STANCE_TIRE_MIN, STANCE_TIRE_MAX, 5.0f, v_tw, m_tw, l_tw, 0 },
};

static struct { int going; uint32_t scr, page_was, handle; long since; } G;   /* from the Body Shop to the page and back */
#define FADE_FRAMES    300           /* a fade the game never ended within this: none (the Body Shop's Stance again) */

/* the Body Shop list: our entry after Rims (0x545C4B, esi the screen) */
static void add_entry(CPU *c){
    uint32_t scr = c->esi, sz = 0x40u;
    if (!guest_ok_bytes(scr, P_BUSY + 1u)) return;
    uint32_t it = gcall(c, FN_NEW, 0, 1, &sz);
    if (!it) return;
    for (uint32_t o = 0; o < 0x40u; o += 4u) MR32(it + o) = 0;
    MR32(it) = VT_PART_ITEM; MR32(it + I_CODE) = STANCE_CODE; MR32(it + I_LABEL) = label_hash();
    MR8(it + 0x21u) = 1; MR8(it + 0x22u) = 0; MR8(it + 0x23u) = 1; MR8(it + 0x34u) = 1;
    MR32(it + 0x38u) = it + 0x38u; MR32(it + 0x3cu) = it + 0x38u;                 /* its own list: empty */
    uint32_t a[3] = { it, 1u, 0xffffffffu }, icon = gcall(c, FN_ITEM_ADD, scr + P_LIST, 3, a);
    if (icon && MR32(icon + 0x24u) != ICON_STANCE){ MR32(icon + 0x24u) = ICON_STANCE; MR32(icon + 0x1cu) |= 0x400000u; }
    MR32(scr + P_COUNT) += 1u;
}

/* the Body Shop list's messages: our entry's Accept, its fade's end */
static void swallow(CPU *c){ c->eax = 1; POP32(c->eip); c->esp += 0x10u; }
static void parts_msg(CPU *c){
    uint32_t scr = c->ecx, msg = MR32(c->esp + 4u), sel = guest_ok_bytes(scr, P_BUSY + 1u) ? MR32(scr + P_SELECTED) : 0;
    if ((msg == MSG_ACCEPT || msg == MSG_ACCEPT2) && stance_on() && guest_ok_bytes(sel, 0x30u) && MR32(sel) == VT_PART_ITEM &&
        MR32(sel + I_CODE) == STANCE_CODE && g_label && MR32(sel + I_LABEL) == g_label){
        if (!MR8(scr + P_BUSY) && !G.going){
            G.going = 1; G.scr = scr; G.since = autotest_frame();
            gcall(c, FN_GO_FADE, scr, 0, 0);
            uint32_t a[2] = { 8u, MR32(scr + 0x30u) };
            gcall(c, FN_FE_SOUND, 0, 2, a);
            fprintf(stderr, "[stance] f=%ld Body Shop: Stance\n", autotest_frame());
        }
        swallow(c); return;
    }
    if (msg == MSG_FADE_DONE && G.going && scr == G.scr){
        G.going = 0;
        MR8(G_RIMS_GO) = 0;
        G.page_was = MR32(G_OPT_PAGE); MR32(G_OPT_PAGE) = PAGE_STANCE;
        G.handle = driven(0);
        const StanceV *had = stance_mine(0);
        M.game_fc = game_camber(G_FE_RIDE);
        if (had) M.v = *had;
        else { memset(&M.v, 0, sizeof M.v); M.v.fc = (int16_t)lroundf(M.game_fc * 10); M.v.tw = 100; }
        M.open = 1;
        fprintf(stderr, "[stance] f=%ld the car %08x: camber %d/%d, offset %d/%d mm, tires %u %% (%s; the game's camber %.1f)\n",
                autotest_frame(), G.handle, M.v.fc, M.v.rc, M.v.fo, M.v.ro, M.v.tw, had ? "its own" : "the game's", M.game_fc);
        uint32_t a[3] = { MR32(scr + 4u), PKG_OPTIONS, 0 };
        gcall(c, FN_SWITCH, 0, 3, a);
        swallow(c); return;
    }
    sub_00566490(c);
}

/* the Options screen's page: its title and our sliders (0x4B85ED, esi the screen), the camera on the car */
static void fill_page(CPU *c){
    uint32_t scr = c->esi;
    uint32_t a[3] = { MR32(scr + 4u), OBJ_TITLE, label_hash() };
    gcall(c, FN_TEXT, 0, 3, a);
    for (int i = 0; i < 5; i++) optslider_add(c, scr, FN_OPT_ADD, &g_sl[i]);
    uint32_t v[4] = { VIEW_FRONT, 0, 0, 1u };
    gcall(c, FN_CAMERA, 0, 4, v);
}

/* the Options screen's messages while it is ours: its Back to the Body Shop */
static void options_msg(CPU *c){
    uint32_t scr = c->ecx, msg = MR32(c->esp + 4u);
    if (!M.open){ sub_0049c690(c); return; }
    if (msg == MSG_DEFAULTS || msg == MSG_ACCEPT2){ swallow(c); return; }
    if (msg == MSG_FADE_DONE && guest_ok_bytes(scr, O_BACK + 4u) && MR32(scr + O_BACK) == MSG_BACK_DONE){
        M.open = 0;
        MR32(G_OPT_PAGE) = G.page_was;
        StanceV stock = { (int16_t)lroundf(M.game_fc * 10), 0, 0, 0, 100 };
        int same = !memcmp(&M.v, &stock, sizeof stock);
        if (G.handle) set_mine(0, G.handle, same ? 0 : &M.v);
        fprintf(stderr, "[stance] f=%ld the car %08x: %s\n", autotest_frame(), G.handle, same ? "the game's stance" : "a stance of its own");
        uint32_t b[2] = { 0, 1u };
        gcall(c, FN_CAMERA_BACK, 0, 2, b);
        MR32(ROW_PARTS_CODE) = STANCE_CODE;     /* the list made again on Stance, as on Rims after its screens (0x545C97) */
        uint32_t a[3] = { MR32(scr + 4u), PKG_PARTS, 0 };
        gcall(c, FN_SWITCH, 0, 3, a);
        swallow(c); return;
    }
    sub_0049c690(c);
}

/* ---- the page's look (test aids: NFSU2_TEST_STANCE_DUMP lists the page's objects, NFSU2_TEST_STANCE_HIDE=<hash>,...
 * hides those; script runs only) ----------------------------------------------------------------------------------- */
static int test_env(const char *nm, const char **v){
    const char *sc = getenv("NFSU2_INPUT_SCRIPT");
    *v = getenv(nm);
    return sc && *sc && *v && **v;
}
static void dump_objs(uint32_t first, int depth, int *n){
    for (uint32_t it = MR32(first); it && *n < 400 && guest_ok(it, 0x68u); it = MR32(it + 4u)){
        uint32_t d = MR32(it + 0x2cu), ty = MR32(it + 0x18u);
        char line[400]; int o = snprintf(line, sizeof line, "[stance] obj %3d %*s%08x type %u flags %08x", (*n)++, depth * 2, "", MR32(it + 0x10u), ty, MR32(it + 0x1cu));
        if (guest_ok(d, 0x54u)){
            o += snprintf(line + o, sizeof line - (size_t)o, " col %08x %08x %08x %08x", MR32(d), MR32(d + 4u), MR32(d + 8u), MR32(d + 12u));
            for (uint32_t k = 0x10u; k < 0x54u && o < (int)sizeof line - 16; k += 4u) o += snprintf(line + o, sizeof line - (size_t)o, " %.3g", MRF32(d + k));
        }
        fprintf(stderr, "%s\n", line);
        if (ty == 5u && depth < 6) dump_objs(it + 0x64u, depth + 1, n);
    }
}
static void hide_hashes(uint32_t first, const char *list, int depth){
    for (uint32_t it = MR32(first); it && guest_ok(it, 0x68u); it = MR32(it + 4u)){
        char hx[12]; snprintf(hx, sizeof hx, "%08x", MR32(it + 0x10u));
        if (strstr(list, hx) && !(MR32(it + 0x1cu) & 1u)){ uint32_t a = it; (void)a; MR32(it + 0x1cu) |= 0x400001u; }
        if (MR32(it + 0x18u) == 5u && depth < 6) hide_hashes(it + 0x64u, list, depth + 1);
    }
}
/* an object of the list first (a package's +0x4C, a group's +0x64) by its hash, 0: none */
static uint32_t child(uint32_t first, uint32_t hash){
    int n = 0;
    for (uint32_t it = MR32(first); it && n < 512 && guest_ok(it, 0x68u); it = MR32(it + 4u), n++) if (MR32(it + 0x10u) == hash) return it;
    return 0;
}
/* the alpha of object o's colour (its drawing data +0xC) at most a; as the FE's setters mark a change */
static void alpha_at_most(uint32_t o, uint32_t a){
    uint32_t d = o ? MR32(o + 0x2cu) : 0;
    if (!guest_ok(d, 0x10u) || MR32(d + 0xcu) <= a) return;
    MR32(d + 0xcu) = a; MR32(o + 0x1cu) |= 0x400000u;
}
/* the Visual Rating's package (the shops' overlay): its objects hidden while the page is open, back after */
#define OBJS_HID 64
static struct { uint32_t pk, o[OBJS_HID]; int n; } g_rating_hid;
static void rating_hide(CPU *c, uint32_t pkname){
    uint32_t pk = gcall(c, FN_FE_PACKAGE, 0, 1, &pkname);
    if (!pk || g_rating_hid.pk == pk) return;
    g_rating_hid.pk = pk; g_rating_hid.n = 0;
    int n = 0;
    for (uint32_t it = MR32(pk + 0x4cu); it && n < 512 && g_rating_hid.n < OBJS_HID && guest_ok(it, 0x68u); it = MR32(it + 4u), n++)
        if (!(MR32(it + 0x1cu) & 1u)){ uint32_t a = it; gcall(c, FN_FE_HIDE, 0, 1, &a); g_rating_hid.o[g_rating_hid.n++] = it; }
}
static int in_package(uint32_t pk, uint32_t o){
    int n = 0;
    for (uint32_t it = MR32(pk + 0x4cu); it && n < 512 && guest_ok(it, 0x68u); it = MR32(it + 4u), n++) if (it == o) return 1;
    return 0;
}
static void rating_show(CPU *c, uint32_t pkname){
    uint32_t pk = g_rating_hid.pk && pkname ? gcall(c, FN_FE_PACKAGE, 0, 1, &pkname) : 0;
    for (int i = 0; pk == g_rating_hid.pk && i < g_rating_hid.n; i++){
        uint32_t a = g_rating_hid.o[i];
        if (in_package(pk, a)) gcall(c, FN_FE_SHOW, 0, 1, &a);
    }
    g_rating_hid.pk = 0; g_rating_hid.n = 0;
}
void stance_frame(CPU *c){
    static int dumped, was_open, gone; static uint32_t pkname, rpkname;
    if (!stance_on()) return;
    if (G.going && autotest_frame() - G.since > FADE_FRAMES){
        G.going = 0; MR8(G_RIMS_GO) = 0;
        fprintf(stderr, "[stance] f=%ld the Body Shop's fade to Stance never ended: not going\n", autotest_frame());
    }
    if (!pkname){
        uint32_t sz = 32u;
        if (!(pkname = gcall(c, FN_NEW, 0, 1, &sz)) || !(rpkname = gcall(c, FN_NEW, 0, 1, &sz))){ pkname = 0; return; }
        guest_write_str(pkname, "UI_Options.fng", 32); guest_write_str(rpkname, PKG_RATING, 32);
    }
    if (!M.open){
        dumped = 0;
        if (was_open){ was_open = 0; rating_show(c, rpkname); }
        return;
    }
    was_open = 1;
    rating_hide(c, rpkname);
    uint32_t pk = gcall(c, FN_FE_PACKAGE, 0, 1, &pkname);
    if (!pk){                                    /* the page gone another way than its Back: the menu's car as before */
        if (++gone > FADE_FRAMES){
            M.open = 0; gone = 0; MR32(G_OPT_PAGE) = G.page_was;
            fprintf(stderr, "[stance] f=%ld the Stance page went without its Back: the car's stance as it was\n", autotest_frame());
        }
        return;
    }
    gone = 0;
    /* the panel's two backings, faint: the car behind them seen */
    static int cap = -1;
    const char *v;
    if (cap < 0) cap = test_env("NFSU2_TEST_STANCE_PANEL", &v) ? (int)strtol(v, 0, 0) : PANEL_ALPHA;
    uint32_t box = child(pk + 0x4cu, OBJ_PANEL);
    if (box){ alpha_at_most(child(box + 0x64u, OBJ_PANEL_OUT), (uint32_t)cap); alpha_at_most(child(box + 0x64u, OBJ_PANEL_IN), (uint32_t)cap); }
    if (dumped++ == 30 && test_env("NFSU2_TEST_STANCE_DUMP", &v)){ int n = 0; dump_objs(pk + 0x4cu, 0, &n); }
    if (test_env("NFSU2_TEST_STANCE_HIDE", &v)) hide_hashes(pk + 0x4cu, v, 0);
}

int stance_hook(CPU *c, uint32_t ip){
    switch (ip){
    case HOOK_SITE(0x00545c4b): if (stance_on()) add_entry(c); return 1;
    case HOOK_SITE(0x004b85ed): if (M.open && MR32(G_OPT_PAGE) == PAGE_STANCE) fill_page(c); return 1;
    }
    return 0;
}

void stance_install(void){
    dispatch_patch(0x00566490u, parts_msg);
    dispatch_patch(0x0049c690u, options_msg);
}
