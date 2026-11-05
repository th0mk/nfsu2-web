/* shownames.c -- Show Player Names, the game's own row of its LAN and Online
 * options (UI_OLMAIN's Options, only a LAN lobby gets there), in Options >
 * Gameplay too, before Save, in the main menu's and the pause menu's: the
 * names over other players' cars on or off (free roam's, roam_game.c's tags;
 * a LAN race's, the game's own).  The game keeps it in 0x8500C9 (1: on, the
 * original's default, which the FEDatabase's constructor sets with its
 * online settings, 0x5030E0), saves it in the profile with them (0x515BF0;
 * a profile's load 0x515C90 sets it), and nothing else writes it but the
 * row (its method 1, 0x49AC20, flips it on a left or a right).
 *
 * The row is the game's own class (vtable 0x791B70, a toggle of 0x54 bytes
 * made by 0x51E230; its method 3, 0x49AC60, writes the name, 0x0B660729,
 * and On or Off, as each language has them), so nothing of ours runs in it.
 * The value goes to the registry (ShowPlayerNames: the device's, as
 * Power Units'), when a row changes it, the LAN lobby's too, and comes back
 * from there after the constructor and after each profile's load, whatever
 * the profile saved; none (until a row changes it): the game's, as in the
 * original.  The screens' lists place an entry's parts by name and number
 * as powerunits.c says: the row goes in only where the screen's package has
 * them for the list's last entry too, which ours makes one more. */
#include "cpu.h"
#include "widescreen.h"           /* HOOK_SITE */
#include <stdio.h>
#include <string.h>

void dispatch_indirect(CPU *c, uint32_t addr);
int  win32_reg_get_dword(const char *nm, uint32_t *out);
void win32_reg_put_dword(const char *nm, uint32_t v);

#define REG_NAME      "ShowPlayerNames"
#define A_NAMES       0x008500c9u   /* byte: Show Player Names, 1 = on */
#define FN_NEW        0x00575620u   /* cdecl (size): operator new */
#define FN_TOGGLE     0x0051e230u   /* thiscall (screen +0xFC, +0x10C, +0x114, +0x11C, 1): a toggle entry */
#define FN_ADD        0x0054ef30u   /* thiscall screen (entry, 1): Options > Gameplay's list takes a toggle */
#define FN_ADD_PAUSE  0x0054f170u   /* thiscall screen (entry, 1): the pause menu's */
#define FN_FE_HASH    0x00505450u   /* cdecl (name) -> hash */
#define FN_FE_PACKAGE 0x0052cef0u   /* cdecl (package name) -> FEPackage* */
#define FN_PKG_FIND   0x00517f80u   /* thiscall package (hash) -> its object, 0: none */
#define VT_NAMES      0x00791b70u   /* Show Player Names' class (UI_OLMAIN's Options) */
#define MSG_LEFT      0x9120409eu
#define MSG_RIGHT     0xb5971bf1u
#define S_COUNT       0x12cu        /* the screen: its entries so far (the next one's number) */
#define S_PACKAGE     4u            /* its package's name */

static int g_loaded;
static int g_on = -1;                            /* 1: on, 0: off, -1: the game's (no registry value) */
static uint32_t g_scr;                           /* guest memory: scratch for a part's name */

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
static void gstr(uint32_t a, const char *s, uint32_t cap){
    uint32_t i = 0;
    for (; s[i] && i + 1 < cap; i++) MR8(a + i) = (uint8_t)s[i];
    MR8(a + i) = 0;
}

static void load(void){
    if (g_loaded) return;
    g_loaded = 1;
    uint32_t v;
    if (win32_reg_get_dword(REG_NAME, &v)) g_on = v != 0;
}
/* the constructor's defaults or a profile's value set: the registry's instead */
static void apply(const char *after){
    load();
    if (g_on < 0 || MR8(A_NAMES) == (uint8_t)g_on) return;
    MR8(A_NAMES) = (uint8_t)g_on;
    fprintf(stderr, "[names] Show Player Names %s (registry %s), after %s\n", g_on ? "On" : "Off", REG_NAME, after);
}
/* a row's method 1 after its flip (every message comes here, eax the message):
 * a left or right's value to the registry */
static void changed(CPU *c){
    if (c->eax != MSG_LEFT && c->eax != MSG_RIGHT) return;
    load();
    g_on = MR8(A_NAMES) != 0;
    win32_reg_put_dword(REG_NAME, (uint32_t)g_on);
    fprintf(stderr, "[names] Show Player Names %s\n", g_on ? "On" : "Off");
}

/* ---- the row ------------------------------------------------------------- */
/* a part's object in the package by name and number, 0: none */
static uint32_t part(CPU *c, uint32_t pk, uint32_t base, uint32_t n, char *nm, size_t cap){
    int k = 0;
    for (; k < 32 && MR8(base + (uint32_t)k); k++) nm[k] = (char)MR8(base + (uint32_t)k);
    snprintf(nm + k, cap - (size_t)k, "%u", n);
    gstr(g_scr, nm, 48);
    uint32_t h = gcall(c, FN_FE_HASH, 0, 1, &g_scr);
    return gcall(c, FN_PKG_FIND, pk, 1, &h);
}
/* Has the screen's package every part for entry n that it has for n - 1?
 * (powerunits.c's: +0xC4 OPTION_NAME_ .. +0xD8, but for +0xCC in the
 * menus' list, 0x54EF30) */
static int parts_there(CPU *c, uint32_t scr, uint32_t n, int pause){
    uint32_t pn = MR32(scr + S_PACKAGE), pk = pn ? gcall(c, FN_FE_PACKAGE, 0, 1, &pn) : 0;
    if (!pk || n < 2u) return 0;
    for (uint32_t o = 0xc4u; o <= 0xd8u; o += 4u){
        char nm[48], was[48];
        uint32_t b = o == 0xccu && !pause ? 0 : MR32(scr + o);
        if (b && !part(c, pk, b, n, nm, sizeof nm) && part(c, pk, b, n - 1u, was, sizeof was)){
            fprintf(stderr, "[names] the screen has %s but no %s: no Show Player Names row\n", was, nm);
            return 0;
        }
    }
    return 1;
}
/* the game's row, entry n, before `after` more of the game's */
static void add(CPU *c, uint32_t scr, uint32_t fn_add, uint32_t after){
    if (!g_scr){
        uint32_t sz = 64u;
        if (!(g_scr = gcall(c, FN_NEW, 0, 1, &sz))) return;
    }
    uint32_t n = MR32(scr + S_COUNT);
    if (!parts_there(c, scr, n + after, fn_add == FN_ADD_PAUSE)) return;
    uint32_t sz = 0x54u, w = gcall(c, FN_NEW, 0, 1, &sz);
    if (!w) return;
    uint32_t a[5] = { scr + 0xfcu, scr + 0x10cu, scr + 0x114u, scr + 0x11cu, 1u };
    gcall(c, FN_TOGGLE, w, 5, a);
    MR32(w) = VT_NAMES;
    uint32_t b[2] = { w, 1u };
    gcall(c, fn_add, scr, 2, b);
}

int shownames_hook(CPU *c, uint32_t ip){
    switch (ip){
    case HOOK_SITE(0x004b7544): add(c, c->esi, FN_ADD, 1u); return 1;         /* Options > Gameplay: after Race Map Mode, before Save */
    case HOOK_SITE(0x004c3bfb): add(c, c->esi, FN_ADD_PAUSE, 1u); return 1;   /* the pause menu's: after Race Map Mode, before Save */
    case HOOK_SITE(0x0049ac46): changed(c); return 1;                         /* the row's method 1, after its flip */
    case HOOK_SITE(0x0053ed7c): apply("the defaults"); return 1;              /* the FEDatabase's constructor: its online settings' defaults */
    case HOOK_SITE(0x0053465f): apply("a profile's load"); return 1;          /* a profile's load (0x5344D0): its online settings */
    }
    return 0;
}
