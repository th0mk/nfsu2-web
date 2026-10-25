/* hornmenu.c -- the horns' volume (horn.c) on the game's own Audio options:
 * a Horn Volume slider after Engine Volume, in the menus' Options > Audio
 * (0x4B6E60, before its Audio Mode) and in the pause menu's (0x4C3370, where
 * Engine Volume is the last), from 10 % to 100 % in steps of 5 %.  Moving it
 * sounds the player's own horn for a moment at the new volume.  The value is
 * the registry's HornVolume (percent; registry.txt, the game's options),
 * read at startup; none: 100 %.
 *
 * The game's volume sliders are a class each over one widget (0x98 bytes,
 * made by 0x51E840 from the screen's layout).  Three methods are their own,
 * the rest shared; Engine Volume's (vtable 0x7919E8):
 *    1  0x49A900  (?, message): its slider (+0x54) takes a left or right
 *                 (0x9120409E, 0xB5971BF1), the arrow blinks (16), the
 *                 value (18: +0x84) goes to the audio settings (0x83AA28
 *                 +0x14) and the sound (0x469050), the label again (3)
 *    3  0x49A950  the label: its text by hash ("Engine Volume")
 *   17  0x49A970  the range and step (+0x7C, +0x78, +0x8C: 0 .. 1, 0.05),
 *                 then the value from the audio settings
 * Ours is a copy of that vtable in guest memory with those three here, at
 * guest addresses nothing runs (the nops after Engine Volume's own three),
 * which the dispatch table sends to this file.  The screen's list (0x54F410;
 * the pause menu's 0x54F800 in front of it) finds an entry's parts in the
 * screen's package by name and number (OPTION_NAME_8, SLIDER_KNOB_8, ...),
 * and places them in its row itself: the menus' UI_Options.fng has them for
 * an eighth slider, the pause menu's UI_PauseOptions.fng all but the bar's,
 * which are copies of the seventh's here (parts_ready). */
#include "cpu.h"
#include "horn.h"
#include "widescreen.h"           /* HOOK_SITE */
#include <math.h>
#include <stdio.h>
#include <string.h>

void dispatch_patch(uint32_t a, void (*f)(CPU *));
void dispatch_indirect(CPU *c, uint32_t addr);
int  win32_reg_get_dword(const char *nm, uint32_t *out);
void win32_reg_put_dword(const char *nm, uint32_t v);
const char *language_name(void);

#define REG_NAME      "HornVolume"
#define FN_NEW        0x00575620u   /* cdecl (size): operator new */
#define FN_WIDGET     0x0051e840u   /* thiscall (screen +0xFC, +0x10C, +0x114, +0x11C, 1): a slider entry */
#define FN_ADD        0x0054f410u   /* thiscall screen (entry, 1): the Options list takes a slider */
#define FN_ADD_PAUSE  0x0054f800u   /* thiscall screen (entry, 1): the pause menu's */
#define FN_SET        0x004ff550u   /* thiscall slider (float): its value, within its range */
#define FN_FE_HASH    0x00505450u   /* cdecl (name) -> hash */
#define FN_FE_PACKAGE 0x0052cef0u   /* cdecl (package name) -> FEPackage* */
#define FN_PKG_FIND   0x00517f80u   /* thiscall package (hash) -> its object, 0: none */
#define FN_FE_SETSTR  0x00535fe0u   /* thiscall string object (char*): its text */
#define VT_ENGINE     0x007919e8u   /* Engine Volume's class */
#define VT_N          19
#define M_ACT         0x0049a947u   /* our methods 1, 3 and 17 */
#define M_LABEL       0x0049a962u
#define M_INIT        0x0049a99fu
#define S_COUNT       0x12cu        /* the screen: its entries so far (the next one's number) */
#define S_PACKAGE     4u            /* its package's name */
#define W_SLIDER      0x54u         /* the entry: its slider (its own vtable) */
#define W_LABEL       0x2cu         /* its OPTION_NAME_n */
#define W_MAX         0x78u
#define W_MIN         0x7cu
#define W_VALUE       0x84u
#define W_STEP        0x8cu

static int g_installed;
static uint32_t g_vt, g_scr;                     /* guest memory: our vtable, then scratch for strings */
static uint32_t g_pct = 100;                     /* the volume, percent, as the registry has it */

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
static uint32_t fbits(float f){ uint32_t u; memcpy(&u, &f, 4); return u; }

/* the label in the game's language: the Latin-script texts as the language
 * files write their own sliders' ("Volume moteur", "Motorvolume", ...; in
 * Latin-1, as they are; Polish's "Glosnosc silnika" with the letters its
 * pack's fonts draw as Polish ones: l-stroke 0xEE, s-acute 0xE2, c-acute
 * 0xE9; Turkish's "Korna Sesi" after its "Motor Sesi", ASCII); English for
 * the others, whose fonts have their own encodings but draw ASCII as it is
 * (car names) */
static const char *label(void){
    static const struct { const char *lang, *text; } T[] = {
        { "french", "Volume klaxon" }, { "german", "Hupenlautst\xe4rke" }, { "italian", "Volume clacson" },
        { "spanish", "Volumen claxon" }, { "dutch", "Claxonvolume" }, { "swedish", "Tutvolym" },
        { "danish", "Lydstyrke for horn" }, { "portuguese", "Volume da Buzina" },
        { "polish", "G\xeeo\xe2no\xe2\xe9 klaksonu" }, { "turkish", "Korna Sesi" },
    };
    const char *l = language_name();
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcmp(l, T[i].lang)) return T[i].text;
    return "Horn Volume";
}

/* 1: a left or right moved the slider; the volume to the horns, the registry and a honk */
static void hv_act(CPU *c){
    uint32_t self = c->ecx, msg = MR32(c->esp + 8u), sl = self + W_SLIDER;
    float was = MRF32(self + W_VALUE);
    if (gcall(c, MR32(MR32(sl) + 4u), sl, 1, &msg) & 0xffu) gcall(c, MR32(MR32(self) + 0x40u), self, 1, &msg);
    float v = MRF32(self + W_VALUE);
    if (v != was){
        uint32_t pct = (uint32_t)lrintf(v * 100.f);
        if (pct < 10u) pct = 10u;
        if (pct > 100u) pct = 100u;
        horn_set_volume((float)pct / 100.f);
        if (pct != g_pct){ g_pct = pct; win32_reg_put_dword(REG_NAME, pct); fprintf(stderr, "[horn] volume %u %%\n", pct); }
        horn_preview();
    }
    gcall(c, MR32(MR32(self) + 0x0cu), self, 0, 0);
    POP32(c->eip); c->esp += 8u;
}
/* 3: the label */
static void hv_label(CPU *c){
    uint32_t obj = MR32(c->ecx + W_LABEL);
    if (obj && g_scr){
        gstr(g_scr, label(), 64);
        gcall(c, FN_FE_SETSTR, obj, 1, &g_scr);
        MR32(obj + 0x1cu) |= 2u;                 /* a text of its own, as FEPrintf leaves it (0x537B30) */
    }
    POP32(c->eip);
}
/* 17: 10 % .. 100 % in steps of 5 %, at the volume */
static void hv_init(CPU *c){
    uint32_t self = c->ecx, v = fbits((float)g_pct / 100.f);
    MRF32(self + W_MAX) = 1.f; MRF32(self + W_MIN) = HORN_VOL_MIN; MRF32(self + W_STEP) = 0.05f;
    gcall(c, FN_SET, self + W_SLIDER, 1, &v);
    POP32(c->eip);
}

/* the registry's volume to the horns, and our methods to the dispatch table (once) */
void hornmenu_install(void){
    if (g_installed) return;
    g_installed = 1;
    uint32_t pct;
    if (win32_reg_get_dword(REG_NAME, &pct)){
        g_pct = pct < 10u ? 10u : pct > 100u ? 100u : pct;
        horn_set_volume((float)g_pct / 100.f);
        fprintf(stderr, "[horn] volume %u %% (registry %s)\n", g_pct, REG_NAME);
    }
    dispatch_patch(M_ACT, hv_act);
    dispatch_patch(M_LABEL, hv_label);
    dispatch_patch(M_INIT, hv_init);
}

/* ---- the entry's parts ------------------------------------------------------ */
static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n); }
/* the address of the object list's "first" (a package's +0x4C, a group's
 * +0x64) that holds o */
static uint32_t fe_list_of(uint32_t first, uint32_t o, int depth){
    int n = 0;
    for (uint32_t it = MR32(first); it && n < 4096; it = MR32(it + 4u), n++){
        if (it == o) return first;
        if (depth < 8 && MR32(it + 0x18u) == 5u){ uint32_t l = fe_list_of(it + 0x64u, o, depth + 1); if (l) return l; }
    }
    return 0;
}
/* a copy of the package's object o under another name's hash, after it in
 * its list: as roam_game.c's fe_clone makes them (Clone, vtable slot 1, then
 * the drawing data an image or a string needs to be drawn at all) */
static uint32_t fe_copy(CPU *c, uint32_t pk, uint32_t o, uint32_t hash){
    uint32_t l = ok(pk, 0x54) && ok(o, 0x5c) && ok(MR32(o), 8) ? fe_list_of(pk + 0x4cu, o, 0) : 0;
    if (!l) return 0;
    uint32_t n = 0, last = 0;
    for (uint32_t it = MR32(l); it && n < 4096; it = MR32(it + 4u)){ last = it; n++; }
    if (n != MR32(l - 4u) || last != MR32(l + 4u)) return 0;          /* not the count and last we take it for */
    uint32_t one = 1, cl = gcall(c, MR32(MR32(o) + 4u), o, 1, &one);
    if (!ok(cl, 0x5c)) return 0;
    MR32(cl + 0x58u) = 0;
    uint32_t dn = MR32(o + 0x30u), d0 = MR32(o + 0x2cu), d1 = MR32(cl + 0x2cu), ty = MR32(o + 0x18u);
    if ((ty == 1u || ty == 2u) && MR32(cl + 0x30u) == dn && dn && dn <= 0x100u && ok(d0, dn) && ok(d1, dn) && d0 != d1)
        for (uint32_t i = 0; i < dn; i++) MR8(d1 + i) = MR8(d0 + i);
    MR32(cl + 0x10u) = hash;
    uint32_t nx = MR32(o + 4u);
    MR32(cl + 4u) = nx; MR32(cl + 8u) = o; MR32(o + 4u) = cl;
    if (nx) MR32(nx + 8u) = cl; else MR32(l + 4u) = cl;
    MR32(l - 4u) += 1;
    return cl;
}
/* The copies and their originals, while the package holds them: the copies
 * take the originals' colour every frame (hornmenu_frame).  They are made as
 * the screen is set up, before the package's scripts have given its objects
 * their colours (a copy then: alpha 0, never drawn, 0x51A970), and their
 * own scripts, copied as they are, never do. */
#define COPIES 10
static struct { uint32_t pk, pkname, n, o[COPIES], cl[COPIES], ho[COPIES], hc[COPIES]; } g_cp;
void hornmenu_frame(CPU *c){
    if (!g_cp.n) return;
    uint32_t pk = gcall(c, FN_FE_PACKAGE, 0, 1, &g_cp.pkname);
    for (uint32_t i = 0; i < g_cp.n && pk == g_cp.pk; i++)              /* each still the package's, by its name too */
        if (!ok(g_cp.o[i], 0x5c) || !ok(g_cp.cl[i], 0x5c) || MR32(g_cp.o[i] + 0x10u) != g_cp.ho[i] || MR32(g_cp.cl[i] + 0x10u) != g_cp.hc[i]
            || !fe_list_of(pk + 0x4cu, g_cp.o[i], 0) || !fe_list_of(pk + 0x4cu, g_cp.cl[i], 0)) pk = 0;
    if (pk != g_cp.pk){ g_cp.n = 0; return; }                         /* the screen is gone, and its objects */
    for (uint32_t i = 0; i < g_cp.n; i++){
        uint32_t d0 = MR32(g_cp.o[i] + 0x2cu), d1 = MR32(g_cp.cl[i] + 0x2cu), ch = 0;
        if (!ok(d0, 16) || !ok(d1, 16)) continue;
        for (uint32_t k = 0; k < 16u; k += 4u) if (MR32(d1 + k) != MR32(d0 + k)){ MR32(d1 + k) = MR32(d0 + k); ch = 1; }
        if (ch) MR32(g_cp.cl[i] + 0x1cu) |= 0x400000u;                 /* changed, as the FE's setters mark it */
    }
}
/* a part's object in the package by name and number, 0: none; *hash: that name's */
static uint32_t part(CPU *c, uint32_t pk, uint32_t base, uint32_t n, uint32_t *hash, char *nm, size_t cap){
    int k = 0;
    for (; base && k < 32 && MR8(base + (uint32_t)k); k++) nm[k] = (char)MR8(base + (uint32_t)k);
    snprintf(nm + k, cap - (size_t)k, "%u", n);
    gstr(g_scr, nm, 48);
    *hash = gcall(c, FN_FE_HASH, 0, 1, &g_scr);
    return gcall(c, FN_PKG_FIND, pk, 1, hash);
}
/* The parts the list reads for entry n (the screen's names, +0xC4
 * OPTION_NAME_ .. +0xF0 SLIDER_END_), in the screen's package: each one the
 * package has for entry n - 1 but not for n is a copy of n - 1's (the pause
 * menu's has the slider's bar, knob and ends only for its seven), one it has
 * for neither stays out, as from the entries before (OPTION_BACKING_ in the
 * pause menu: none; SLIDER_VALUE_, a number the slider prints only where
 * there is one).  0: the package is not there. */
static int parts_ready(CPU *c, uint32_t scr, uint32_t n){
    static const uint32_t NAMES[COPIES] = { 0xc4u, 0xd0u, 0xd4u, 0xd8u, 0xdcu, 0xe0u, 0xe4u, 0xe8u, 0xecu, 0xf0u };
    uint32_t pn = MR32(scr + S_PACKAGE), pk = pn ? gcall(c, FN_FE_PACKAGE, 0, 1, &pn) : 0;
    if (!pk || n < 2u) return 0;
    char line[200]; int o_n = 0;
    g_cp.n = 0; g_cp.pk = pk; g_cp.pkname = pn;
    for (unsigned i = 0; i < COPIES; i++){
        char nm[48], was[48];
        uint32_t h, h0, b = MR32(scr + NAMES[i]);
        if (part(c, pk, b, n, &h, nm, sizeof nm)) continue;
        uint32_t o = part(c, pk, b, n - 1u, &h0, was, sizeof was), cl = o ? fe_copy(c, pk, o, h) : 0;
        if (!o) continue;
        if (!cl){ fprintf(stderr, "[horn] no copy of %s for %s: no Horn Volume slider\n", was, nm); return 0; }
        g_cp.o[g_cp.n] = o; g_cp.cl[g_cp.n] = cl; g_cp.ho[g_cp.n] = h0; g_cp.hc[g_cp.n++] = h;
        if (o_n < (int)sizeof line - 40) o_n += snprintf(line + o_n, sizeof line - (size_t)o_n, " %s", nm);
    }
    if (g_cp.n) fprintf(stderr, "[horn] Horn Volume, entry %u: copies of entry %u's parts as%.*s\n", n, n - 1u, o_n, line);
    return 1;
}
static void add(CPU *c, uint32_t scr, uint32_t fn_add){
    hornmenu_install();
    if (!g_vt){
        uint32_t sz = VT_N * 4u + 4u + 64u;
        if (!(g_vt = gcall(c, FN_NEW, 0, 1, &sz))) return;
        for (uint32_t i = 0; i <= VT_N; i++) MR32(g_vt + 4u * i) = MR32(VT_ENGINE + 4u * i);
        MR32(g_vt + 4u * 1u) = M_ACT; MR32(g_vt + 4u * 3u) = M_LABEL; MR32(g_vt + 4u * 17u) = M_INIT;
        g_scr = g_vt + VT_N * 4u + 4u;
    }
    if (!parts_ready(c, scr, MR32(scr + S_COUNT))) return;
    uint32_t sz = 0x98u, w = gcall(c, FN_NEW, 0, 1, &sz);
    if (!w) return;
    uint32_t a[5] = { scr + 0xfcu, scr + 0x10cu, scr + 0x114u, scr + 0x11cu, 1u };
    gcall(c, FN_WIDGET, w, 5, a);
    MR32(w) = g_vt;
    uint32_t b[2] = { w, 1u };
    gcall(c, fn_add, scr, 2, b);
}

int hornmenu_hook(CPU *c, uint32_t ip){
    switch (ip){
    case HOOK_SITE(0x004b7102): add(c, c->esi, FN_ADD); return 1;          /* Options > Audio: after Engine Volume, before Audio Mode */
    case HOOK_SITE(0x004c36a3): add(c, c->esi, FN_ADD_PAUSE); return 1;    /* the pause menu's: after Engine Volume */
    }
    return 0;
}
