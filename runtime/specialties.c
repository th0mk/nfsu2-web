/* specialties.c -- the Car Specialties shop (UI_ICEMAIN.fng): Stock gauges can be
 * picked again, the gauge list opens on the gauge fitted, and Back out of Trunk
 * Audio keeps the shop's list on Trunk Audio.  Changes to the game's own
 * behaviour (v1.2 NA), asked for after Discord reports; nothing else of the
 * shops changes.
 *
 * Custom Gauges (UI_CustomHUDOverlay.fng, made by 0x56C420 through the screen
 * table; its screen's messages 0x563CF0, vtable 0x79D978):
 *  - its constructor (0x56C210) sets the screen's stage [0x838560] to 1 (the
 *    style list; 2-4 the colour pickers, 0 once the colours are picked) and
 *    selects the list's first item, Stock (0x53C1F0).  Enter (0x406415E3, at
 *    0x564135) asks the style's attribute 0x311151F8 (Stock 0, the others
 *    1-10): at a stage above 0 a style with colours opens the colour pickers
 *    and Stock does nothing, so a car that had a gauge bought kept it for good.
 *    At stage 0 the same Enter on a style without colours is the game's own
 *    install: "already installed" when it is fitted, else its install question
 *    (Yes 0x51123592, 0x563EF1: fitted with 0x55C230, no purchase, the colours
 *    [0x839C24], [0x839C28], [0x839C2C] into slots 164, 166 and 165, the car
 *    record written by 0x503950).  Here Enter on Stock in the style list runs
 *    at stage 0, and its Yes puts the colour slots back to each slot's first
 *    part (0x61BA30: WHITE, which a car has there until its gauges are
 *    changed): the gauges as before any purchase, free, in the game's values;
 *  - the list now opens on the style fitted (FE ride 0x8392C0, slot 163
 *    CUSTOM_HUD), selected after the factory as the constructor selects the
 *    first: the list's select (0x5386A0), the item's labels and preview
 *    (0x548D20) and the list's arrows (0x50CA00 hide, 0x50CA50 show).  Its
 *    preview has the list's own colours, as every style the list shows.
 * Trunk Audio is a second UI_ICEMAIN.fng screen (factory 0x560550, messages
 * 0x568740), its one category Trunk Layout.  The category lists keep their
 * place in the screen table's row of their package (UI_ICEMAIN.fng: 0x7F890C,
 * +0x10 the place plus 1, 0 the first; written as the selection moves,
 * 0x543D40, read as the list is made, 0x5593E0), and the Trunk Audio list's
 * Back (0x568C90 -> 0x568CEE) writes 0 there, so the shop's list was made
 * again on Custom Gauges.  Here the place Trunk Audio was opened from is
 * written back before the shop's list is made again.
 * All four functions are reached only through pointers (the screen table, the
 * screens' vtables): dispatch_patch, no change to the lift. */
#include "cpu.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

void dispatch_patch(uint32_t a, void (*f)(CPU *));
void dispatch_indirect(CPU *c, uint32_t addr);
long autotest_frame(void);
void sub_00563cf0(CPU *c);                     /* the lifted ones the patches run */
void sub_0056c420(CPU *c);
void sub_00568740(CPU *c);
void sub_00560550(CPU *c);

#define FN_HUD_MSG      0x00563cf0u   /* thiscall gauge screen (msg, a, b, c), ret 0x10 */
#define FN_HUD_NEW      0x0056c420u   /* cdecl (arg) -> UI_CustomHUDOverlay.fng's screen */
#define FN_ICE_MSG      0x00568740u   /* thiscall UI_ICEMAIN screen (msg, a, b, c), ret 0x10 */
#define FN_ICE_NEW      0x00560550u   /* cdecl (arg) -> UI_ICEMAIN.fng's screen */
#define FN_ATTR         0x0061b500u   /* thiscall part (hash, default) -> attribute */
#define FN_LIST_SELECT  0x005386a0u   /* thiscall list (item) */
#define FN_HUD_SHOW     0x00548d20u   /* thiscall gauge screen (): the current item's labels and preview */
#define FN_HIDE         0x0050ca00u   /* cdecl (object) */
#define FN_SHOW         0x0050ca50u   /* cdecl (object) */
#define FN_PART_FIRST   0x0061ba30u   /* thiscall parts (car type, slot, 0, -1): the slot's first part */
#define G_PARTS         0x008a2b68u   /* the car part database */
#define G_HUD_STAGE     0x00838560u
#define G_FE_RIDE       0x008392c0u   /* the front end's ride of the car: +0 car type, +0x590 + 4 x slot its parts */
#define G_COL_BACKING   0x00839c24u   /* the gauge screen's colours: slot 164 */
#define G_COL_CHAR      0x00839c28u   /* ... 166 */
#define G_COL_NEEDLE    0x00839c2cu   /* ... 165 */
#define G_LIST_KIND     0x00838998u   /* the kind of list UI_ICEMAIN.fng makes (0x5593E0) */
#define ROW_ICE_PLACE   0x007f891cu   /* UI_ICEMAIN.fng's screen-table row (0x7F890C) +0x10 */
#define RIDE_PARTS      0x590u
#define SLOT_HUD        0xa3u
#define SLOT_BACKING    0xa4u
#define SLOT_NEEDLE     0xa5u
#define SLOT_CHAR       0xa6u
#define SLOT_TRUNK      0x22u         /* TRUNK_AUDIO */
#define ATTR_STYLE      0x311151f8u   /* a gauge's style: Stock 0 */
#define MSG_PAD_ACCEPT  0x406415e3u   /* bStringHash("PAD_ACCEPT") */
#define MSG_INSTALL_YES 0x51123592u
#define MSG_BUTTON      0x0c407210u   /* a button pressed (the list's item chosen) */
#define S_FLAG_SEEN     0x129u        /* gauge screen: Enter's first message seen */
#define S_FLAG_BUSY     0x12au
#define S_LIST          0x50u         /* gauge screen: its list; +0x54/+0x58 the first and last node, +0x5C the item */
#define S_CATEGORY      0x110u        /* ... its category: +0x48 slot, +0x4C the part chosen */
#define S_UP            0xfcu         /* ... the list's arrows */
#define S_DOWN          0x100u
#define I_PART          0x2cu         /* a list item: its part (the node is the item +4) */
#define ICE_ITEM        0x58u         /* UI_ICEMAIN screen: the category shown (+0x48 its slot) */

static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n - 1u); }

/* a guest function with ecx and stack arguments, the CPU as it was after */
static uint32_t gcall(CPU *c, uint32_t fn, uint32_t ecx, int n, const uint32_t *arg){
    CPU save = *c;
    c->esp = (c->esp - 256u) & ~15u;
    for (int i = n - 1; i >= 0; i--) PUSH32(arg[i]);
    c->ecx = ecx;
    PUSH32(fn);                                  /* the callee's ret pops it */
    dispatch_indirect(c, fn);
    uint32_t r = c->eax;
    *c = save;
    return r;
}

static int pkg_is(uint32_t s, const char *name){
    size_t n = strlen(name);
    if (!ok(s, (uint32_t)n + 1u)) return 0;
    for (size_t i = 0; i < n; i++) if ((MR8(s + i) | 0x20) != (name[i] | 0x20)) return 0;
    return MR8(s + n) == 0;
}

/* --- Custom Gauges ------------------------------------------------------------ */
/* the gauge screen's current item, when it is a style without colours (Stock) */
static uint32_t stock_item(CPU *c, uint32_t scr){
    uint32_t cat = MR32(scr + S_CATEGORY), item = MR32(scr + S_LIST + 0xcu);
    if (!ok(scr, 0x130u) || MR32(scr + S_LIST + 4u) == scr + S_LIST + 4u || !ok(cat, 0x50u) || MR32(cat + 0x48u) != SLOT_HUD) return 0;
    if (!ok(item, I_PART + 4u)) return 0;
    uint32_t part = MR32(item + I_PART), a[2] = { ATTR_STYLE, 0 };
    if (!ok(part, 14u)) return 0;
    return gcall(c, FN_ATTR, part, 2, a) == 0 ? part : 0;
}

/* a slot's first part for a car type, as the part database lists them (0x61BA30): for the gauges' colours WHITE,
 * what a car has in them before its gauges are changed */
static uint32_t first_part(CPU *c, uint32_t type, uint32_t slot){
    uint32_t a[4] = { type, slot, 0, 0xffffffffu };
    return gcall(c, FN_PART_FIRST, G_PARTS, 4, a);
}
static uint32_t key_of(uint32_t part){ return ok(part, 4u) ? MR32(part) : 0u; }

static void hud_msg(CPU *c){
    uint32_t scr = c->ecx, msg = MR32(c->esp + 4u), stage = MR32(G_HUD_STAGE);
    int lifted = 0;
    if (msg == MSG_PAD_ACCEPT && stage == 1u && MR8(scr + S_FLAG_SEEN) && !MR8(scr + S_FLAG_BUSY) && stock_item(c, scr)){
        MR32(G_HUD_STAGE) = 0;                   /* the game's install of a style without colours */
        lifted = 1;
        fprintf(stderr, "[shop] f=%ld Custom Gauges: Stock picked\n", autotest_frame());
    }
    if (msg == MSG_INSTALL_YES && stock_item(c, scr)){
        uint32_t type = MR32(G_FE_RIDE);
        MR32(G_COL_BACKING) = first_part(c, type, SLOT_BACKING);
        MR32(G_COL_CHAR) = first_part(c, type, SLOT_CHAR);
        MR32(G_COL_NEEDLE) = first_part(c, type, SLOT_NEEDLE);
        fprintf(stderr, "[shop] f=%ld Custom Gauges: Stock fitted, the colours as a new car's (%08x %08x %08x), free\n",
                autotest_frame(), key_of(MR32(G_COL_BACKING)), key_of(MR32(G_COL_NEEDLE)), key_of(MR32(G_COL_CHAR)));
    }
    sub_00563cf0(c);
    if (lifted && MR32(G_HUD_STAGE) == 0u) MR32(G_HUD_STAGE) = stage;
}

/* the gauge list made: on the style fitted, as the constructor selects the first */
static void hud_new(CPU *c){
    sub_0056c420(c);
    uint32_t scr = c->eax, fitted = MR32(G_FE_RIDE + RIDE_PARTS + 4u * SLOT_HUD), cat = scr ? MR32(scr + S_CATEGORY) : 0;
    fprintf(stderr, "[shop] f=%ld Custom Gauges: fitted %08x, colours %08x %08x %08x\n", autotest_frame(), key_of(fitted),
            key_of(MR32(G_FE_RIDE + RIDE_PARTS + 4u * SLOT_BACKING)), key_of(MR32(G_FE_RIDE + RIDE_PARTS + 4u * SLOT_NEEDLE)),
            key_of(MR32(G_FE_RIDE + RIDE_PARTS + 4u * SLOT_CHAR)));
    if (!fitted || !ok(scr, 0x130u) || !ok(cat, 0x50u) || MR32(cat + 0x48u) != SLOT_HUD) return;
    uint32_t head = scr + S_LIST + 4u, node = MR32(head), item = 0;
    for (int k = 0; node && node != head && k < 64 && ok(node, 8u); k++, node = MR32(node))
        if (ok(node - 4u, I_PART + 4u) && MR32(node - 4u + I_PART) == fitted){ item = node - 4u; break; }
    if (!item || item == MR32(scr + S_LIST + 0xcu)) return;
    gcall(c, FN_LIST_SELECT, scr + S_LIST, 1, &item);
    MR32(cat + 0x4cu) = fitted;
    gcall(c, FN_HUD_SHOW, scr, 0, 0);
    uint32_t up = MR32(scr + S_UP), down = MR32(scr + S_DOWN);
    uint32_t first = MR32(scr + S_LIST + 4u) - 4u, last = MR32(scr + S_LIST + 8u) - 4u;
    gcall(c, FN_SHOW, 0, 1, &up);
    gcall(c, FN_SHOW, 0, 1, &down);
    if (item == first) gcall(c, FN_HIDE, 0, 1, &up);
    else if (item == last) gcall(c, FN_HIDE, 0, 1, &down);
    fprintf(stderr, "[shop] f=%ld Custom Gauges: the list on the gauge fitted\n", autotest_frame());
}

/* --- Trunk Audio's Back ------------------------------------------------------- */
static struct { int state; uint32_t at, kind; } g_trunk;   /* 1: opened from the shop's list, 2: its list made */

static void ice_msg(CPU *c){
    uint32_t scr = c->ecx, msg = MR32(c->esp + 4u), it = MR32(scr + ICE_ITEM);
    if (msg == MSG_BUTTON && g_trunk.state != 2 && ok(scr, 0x170u) && pkg_is(MR32(scr + 4u), "UI_ICEMAIN.fng")){
        if (ok(it, 0x4cu) && MR32(it + 0x48u) == SLOT_TRUNK){
            g_trunk.state = 1; g_trunk.at = MR32(ROW_ICE_PLACE); g_trunk.kind = MR32(G_LIST_KIND);
            fprintf(stderr, "[shop] f=%ld Trunk Audio from the list's place %u (list kind %u)\n", autotest_frame(), g_trunk.at, g_trunk.kind);
        } else g_trunk.state = 0;
    }
    sub_00568740(c);
}

static void ice_new(CPU *c){
    uint32_t kind = MR32(G_LIST_KIND);
    if (g_trunk.state == 1) g_trunk.state = kind != g_trunk.kind ? 2 : 0;     /* Trunk Audio's own list, or none came */
    else if (g_trunk.state == 2 && kind == g_trunk.kind){                    /* the shop's list again */
        if (MR32(ROW_ICE_PLACE) == 0u && g_trunk.at){
            MR32(ROW_ICE_PLACE) = g_trunk.at;
            fprintf(stderr, "[shop] f=%ld Trunk Audio's Back: the list stays on its place %u\n", autotest_frame(), g_trunk.at);
        }
        g_trunk.state = 0;
    }
    sub_00560550(c);
}

void specialties_install(void){
    dispatch_patch(FN_HUD_MSG, hud_msg);
    dispatch_patch(FN_HUD_NEW, hud_new);
    dispatch_patch(FN_ICE_MSG, ice_msg);
    dispatch_patch(FN_ICE_NEW, ice_new);
}
