/* vinylcat.c -- the vinyls of the game's hidden category in the vinyl
 * shop's list of categories (Car Customize and the career's Graphics shops,
 * Paint / Vinyls), after Sponsor, for the cars that have any.
 *
 * The game (v1.2 NA; the place as ExtraOptions' ShowSpecialVinyls has it,
 * ExOptsTeam/NFSU2ExOpts, GPL-3.0: only the facts are used here):
 *   PartSelectionScreen::SetupVinyls adds its categories with 0x545920
 *     (thiscall screen: category, icon, label), the last Sponsor (0x16) at
 *     0x546257; HOOK_SITE 0x54625C is the instruction after it (esi the
 *     screen).  A vinyl's category is its part record's byte +5, low 5 bits
 *     (the top 3: its unlock level); category 0x1C has vinyls but no entry.
 *   0x545920 lists the car's vinyls of the category that the shop's lock
 *     test lets through (0x52A010 with 0x50F0D0's filter: the career's stage
 *     unlocks, Customize's own), and shows the category locked when none do,
 *     as the game's own categories.
 *   The career: a car's visual rating (0x5234C0) is its parts' points,
 *     0x501860(0x501D70(slot, part), level), and 0x501D70 gives vinyls of a
 *     category above 0x1B the kind 0, worth 0 points: these vinyls add
 *     nothing to it, as no vinyl at all.
 *   Category 0x1C holds the promo vinyls (STREET REAPER, D3, JAPAN TUNING,
 *     SHINESTREET, CHINGY, CAPONE, DAVID CHOE, SNOOP DOGG, SNOOP DOGG2, THE
 *     DOORS; unlock levels 2 and 3, price 0) and DEBUG VINYL, a texture test
 *     grid ("RIGHT" on the car's sides).
 *   The PC release draws a vinyl from the car's own CARS/<car>/VINYLS.BIN
 *     ("<CAR>_<texture>" and its "_MASK"), and only D3, SHINESTREET, CHINGY
 *     and CAPONE are in every player car's; the other six are made for a
 *     few cars (ONLY below, from each car's VINYLS.BIN): picked on another
 *     car, they draw nothing.
 * Here the category is added only when the shown car has vinyls of it, so
 * a car without them shows no empty, locked category, and DEBUG VINYL and
 * the vinyls the car has no picture of stay out of it: for the call their
 * category byte names none (0x1F), then it is put back. */
#include "cpu.h"
#include "widescreen.h"           /* HOOK_SITE */
#include <stdio.h>
#include <string.h>

void dispatch_indirect(CPU *c, uint32_t addr);
uint32_t language_add_text(const char *name, const char *text);
void language_game_letters(const char *utf8, char *out, size_t n);
const char *language_name(void);
void language_texts_frame(CPU *c);
uint32_t language_text(uint32_t hash);

#define FN_ADD_CAT     0x00545920u   /* thiscall screen (category, icon, label) */
#define FN_PART_FIRST  0x0061ba30u   /* thiscall parts (car, slot, 0, -1): the car's first part of the slot */
#define FN_PART_NEXT   0x0061ba50u   /* thiscall parts (part, car, slot, 0, -1): the next one */
#define G_PARTS        0x008a2b68u   /* the car part database */
#define G_FECAR        0x008389acu   /* the front end's car: vtbl[1] its car type */
#define G_SLOT_KIND    0x00802960u   /* per slot: the kind its parts' byte +4 has (0x60FEE0) */
#define SLOT_VINYL     0x40u
#define CAT_HIDDEN     0x1cu
#define ICON_SPONSOR   0x93ec1ce9u   /* the Sponsor category's icon (the NFSU2 logo) */
#define FN_PART_NAME   0x00610000u   /* thiscall part () -> its name */
#define P_KIND         4u
#define P_CAT          5u
#define CAT_NONE       0x1fu
#define LEFT_OUT       "DEBUG VINYL"
#define NOUT           16
#define G_CARTYPES     0x008a1cccu   /* CarTypeInfo[] (0x890 each): +0 the type's name, as CARS/<name>/ has it */
#define CARTYPES_N     46u

/* the vinyls whose pictures only some cars' VINYLS.BIN have (by the type's name; the texture's name after the car's) */
static const struct { const char *vinyl, *cars; } ONLY[] = {
    { "STREET REAPER", " 3000GT CELICA ESCALADE G35 GTO MUSTANGGT RX8 TT " },   /* STREETREAPER */
    { "JAPAN TUNING",  " G35 " },                                               /* AD_JAPANTUNNING */
    { "DAVID CHOE",    " 350Z COROLLA " },                                      /* DAVIDCHOE */
    { "SNOOP DOGG",    " MUSTANGGT " },                                         /* SNOOPDOGG */
    { "SNOOP DOGG2",   " ESCALADE " },                                          /* SNOOPDOGG2 */
    { "THE DOORS",     " GTO " },                                               /* THEDOORS */
};

static uint32_t g_label;

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

static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n - 1u); }

/* the label, in the game's language: "Special", in each Latin-script language as a vinyl category's word (not the
 * word its Unique category has: Turkish "Ozel" and Hungarian "Kulonleges" are those); Russian, Ukrainian and Korean
 * the first word of the game's own "Special Events" (0x06F8E5BF: "Osobye sobytiya", ...) in its fonts' letters;
 * Chinese, Japanese and Thai, whose texts have no spaces to cut at, the English word (their fonts draw ASCII) */
static uint32_t label(void){
    static const struct { const char *lang, *text; } T[] = {
        { "french", "Sp\xc3\xa9" "ciaux" }, { "german", "Spezial" }, { "italian", "Speciali" }, { "spanish", "Especiales" },
        { "dutch", "Speciaal" }, { "swedish", "Special" }, { "danish", "Specielle" }, { "portuguese", "Especiais" },
        { "polish", "Specjalne" }, { "turkish", "Ekstra" }, { "hungarian", "Speci\xc3\xa1lis" },
        { "czech", "Speci\xc3\xa1ln\xc3\xad" }, { "slovak", "\xc5\xa0peci\xc3\xa1lne" },
    };
    if (g_label) return g_label;
    const char *l = language_name(), *t = "Special";
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcmp(l, T[i].lang)) t = T[i].text;
    char s[64];
    language_game_letters(t, s, sizeof s);
    if (!strcmp(l, "russian") || !strcmp(l, "ukrainian") || !strcmp(l, "korean")){
        uint32_t g = language_text(0x06f8e5bfu);
        size_t n = 0;
        if (g && ok(g, 1u)) while (n + 1 < sizeof s && ok(g + n, 1u) && MR8(g + n) && MR8(g + n) != ' ') n++;
        if (n && ok(g + n, 1u) && MR8(g + n) == ' '){ memcpy(s, g_mem + g, n); s[n] = 0; }
    }
    g_label = language_add_text("VINYL_CATEGORY_SPECIAL_N2", s);
    return g_label;
}

/* the car type's name ("CIVIC"), "" when it can't be read */
static void type_name(uint32_t car, char *out, size_t n){
    uint32_t tab = MR32(G_CARTYPES);
    size_t o = 0;
    out[0] = 0;
    if (car >= CARTYPES_N || !ok(tab, CARTYPES_N * 0x890u)) return;
    for (uint32_t i = 0; i < 0x20u && o + 1 < n; i++){
        uint8_t ch = MR8(tab + car * 0x890u + i);
        if (!ch) break;
        if (ch <= ' ' || ch > '~'){ out[0] = 0; return; }
        out[o++] = (char)ch;
    }
    out[o] = 0;
}

/* a vinyl the car has no picture of: one of ONLY's whose cars it isn't (a car whose name can't be read keeps them) */
static int not_this_car(const char *vinyl, const char *car){
    char pat[40];
    if (!car[0]) return 0;
    snprintf(pat, sizeof pat, " %s ", car);
    for (unsigned i = 0; i < sizeof ONLY / sizeof ONLY[0]; i++)
        if (!strcmp(vinyl, ONLY[i].vinyl)) return !strstr(ONLY[i].cars, pat);
    return 0;
}

/* the car's vinyls of the hidden category (any unlock level), but the left-out ones, which go to out[] */
static int hidden_vinyls(CPU *c, uint32_t car, uint32_t *out, int *nout){
    static uint32_t logged = 0xffffffffu;
    char tn[40];
    type_name(car, tn, sizeof tn);
    uint32_t kind = MR32(G_SLOT_KIND + 4u * SLOT_VINYL), a[5] = { car, SLOT_VINYL, 0, 0xffffffffu, 0 };
    uint32_t p = gcall(c, FN_PART_FIRST, G_PARTS, 4, a);
    int n = 0, guard = 0;
    char list[640]; size_t o = 0; list[0] = 0;
    while (p && ok(p, 14u) && guard++ < 4096){
        if ((int32_t)(int8_t)MR8(p + P_KIND) == (int32_t)kind && (MR8(p + P_CAT) & 0x1fu) == CAT_HIDDEN){
            char nm[32] = {0};
            uint32_t s = gcall(c, FN_PART_NAME, p, 0, 0);
            for (size_t i = 0; s && i + 1 < sizeof nm && ok(s + i, 1u) && MR8(s + i); i++) nm[i] = (char)MR8(s + i);
            int drop = !strcmp(nm, LEFT_OUT) || not_this_car(nm, tn);
            if (drop){ if (*nout < NOUT) out[(*nout)++] = p; else drop = 0; }
            if (!drop) n++;
            if (o + strlen(nm) + 24u < sizeof list)
                o += (size_t)snprintf(list + o, sizeof list - o, " %s/%u%s", nm, MR8(p + P_CAT) >> 5,
                                      !drop ? "" : strcmp(nm, LEFT_OUT) ? " (not this car's)" : " (left out)");
        }
        uint32_t b[5] = { p, car, SLOT_VINYL, 0, 0xffffffffu };
        p = gcall(c, FN_PART_NEXT, G_PARTS, 5, b);
    }
    if (logged != car){
        logged = car;
        fprintf(stderr, "[vinyls] car type %u (%s): %d vinyls of the hidden category (name/level):%s\n", car, tn[0] ? tn : "?", n, list);
    }
    return n;
}

int vinylcat_hook(CPU *c, uint32_t ip){
    if (ip != HOOK_SITE(0x0054625c)) return 0;   /* mov eax, [esi+4]: the categories are in */
    uint32_t fe = MR32(G_FECAR);
    if (!ok(fe, 4u) || !ok(MR32(fe), 8u)) return 1;
    uint32_t car = gcall(c, MR32(MR32(fe) + 4u), fe, 0, 0);
    uint32_t out[NOUT]; uint8_t was[NOUT]; int nout = 0;
    if (hidden_vinyls(c, car, out, &nout) <= 0) return 1;
    int first = !g_label;
    uint32_t a[3] = { CAT_HIDDEN, ICON_SPONSOR, label() };
    if (first) language_texts_frame(c);         /* the label in the game's table before the menu shows it */
    for (int i = 0; i < nout; i++){ was[i] = MR8(out[i] + P_CAT); MR8(out[i] + P_CAT) = (uint8_t)((was[i] & 0xe0u) | CAT_NONE); }
    gcall(c, FN_ADD_CAT, c->esi, 3, a);
    for (int i = 0; i < nout; i++) MR8(out[i] + P_CAT) = was[i];
    return 1;
}
