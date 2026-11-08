/* modpacks.c -- the parts community mods add to the game (tools/modpacks.py,
 * MODPACKS/ in the game folder), for every player alike: everyone's game has
 * them, so a friend's car with a mod's rims looks the same on every screen;
 * a mod's switch (a settings.ini key, the pack names its environment name)
 * only decides whether the shops offer its parts.  Parts already on a car
 * stay, with the switch off too: a save never loses them.  Without
 * MODPACKS/PARTS.BIN nothing changes.  Today the packs hold new rim brands
 * (Blitz), mod_rims = on (NFSU2_MOD_RIMS=1), off by default; for now the
 * page has no row for it (web/src/app/settings.ts).
 *
 * The parts: GLOBALB's car part database (chunk 0x80034602, its loader
 * 0x636BE0: runtime/vinylpacks.c, which hands its copy of the chunk here)
 * gets them at its end, each table its rows: strings, attribute lists,
 * attributes, model records, owners, parts (the layout in vinylpacks.c).
 * Only parts the loader does not group (kinds other than 0x30 decals and
 * 0x32 vinyls, which it counts per category and level as a first part and a
 * count) can go at the end; a model record (0x24 bytes) is a byte the loader
 * tests, one unknown, a u16 string (its base name, e.g. "_STYLE01"), then 8
 * strings (u32, -1 none; the loader makes them pointers).  The game finds a
 * rim's brand by its BRAND_NAME attribute (the brand part's key), the
 * brand's model pack by the brand part's name (CARS\WHEELS\GEOMETRY_<name>.BIN:
 * the front end's 0x57F030 and free roam's part_packs load every brand's),
 * and the models by "<brand><base><size>_A".  Their textures come with the
 * pack's CARS/WHEELS/TEXTURES.BIN (runtime/fileio.c), a texture pack the
 * game keeps loaded.
 *
 * What the game hard-codes about rim brands, here taught the new ones:
 *  - the rim shop's brand list (0x546520): one 0x5463B0 call per brand
 *    (this, brand key, icon texture hash, name text hash; ret 0xC), the game's
 *    in about alphabetical order, Stock last; an SUV's list (car type +0x88A)
 *    has its own few brands.  Here a mod's brands go in before the first of
 *    the game's whose name comes after theirs (before Stock at the latest),
 *    in the cars' list only, while the mod is on; 0x5463B0 greys a brand out
 *    when no rim of it fits the car (as the game's own);
 *  - a brand's name (0x50E960, thiscall 0x838980 (brand key) -> its text,
 *    0 for a key it does not know; the rim screen's title prints it): the
 *    name text of a mod's brand, RIMS_BRAND_<name>, from the pack (the mod's
 *    English text) into the game's language table (language.c). */
#include "cpu.h"
#include "guest.h"
#include "game.h"
#include "setting.h"
#include "stance.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

uint32_t gheap_alloc(int heap, uint32_t n, int zero);
int fileio_mod_pack(const char *rel, char *out, size_t outsz);
uint32_t language_add_text(const char *name, const char *text);
uint32_t language_text(uint32_t hash);
void sub_005463b0_orig(CPU *c);
void sub_0050e960_orig(CPU *c);

#define CHUNK_PARTS   0x80034602u
#define PART_SZ       14u
#define MODEL_SZ      0x24u
#define KIND_DECAL    0x30u
#define KIND_VINYL    0x32u
#define KIND_BRAND    67u           /* a rim brand (the parts of slot 0x43 of car hash 0xA6E1B733) */
#define KEY_STOCK     0x03b83203u   /* the brand list's last entry, Stock (not a part) */
#define RET_LIST      0x00546630u   /* 0x5463B0's return in 0x546520: the cars' list's first brand */
#define RET_LIST_SUV  0x0054656cu   /* the same in an SUV's list */
#define NONE          0xffffffffu

/* MODPACKS/PARTS.BIN (tools/modpacks.py) */
#define PK_HDR        32u
#define PK_MOD        32u
#define PK_PART       128u
#define MAX_MODS      8
#define MAX_PARTS     4096
#define MAX_BRANDS    32

static const uint32_t SUB[7] = { 0x34603u, 0x34606u, 0x3460cu, 0x34605u, 0x3460au, 0x3460bu, 0x34604u };
enum { S_HDR, S_STR, S_LIST, S_ATTR, S_MODEL, S_OWNER, S_PART };

typedef struct {
    uint32_t key, owner, name, mbase, mstr[8], nattr, attr[8][2], stand;
    uint8_t kind, cl, b6, mod, m0, m1, has_model;
} Part;
typedef struct { uint32_t key, tex, text; char name[32]; int mod; } Brand;

static struct {
    int loaded, nmods, nparts;
    char env[MAX_MODS][32];
    int on[MAX_MODS];
    Part *p;
    uint8_t *str; uint32_t nstr;
} g;
static Brand g_brand[MAX_BRANDS]; static int g_nbrand;
static struct { uint32_t key; char name[24]; } g_gbrand[64]; static int g_ngbrand;   /* the game's brands */
static int g_cur = -1;                                 /* the brand list being made: the next brand of ours */
static int g_grown;                                    /* the part database has the packs' parts (else no brand is offered) */

static uint32_t bhash(const char *s){
    uint32_t h = 0xffffffffu;
    for (; *s; s++) h = h * 33u + (uint8_t)*s;
    return h;
}
static uint32_t rd32(const uint8_t *b){ return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static const char *pstr(uint32_t o){ return o < g.nstr ? (const char *)g.str + o : 0; }

/* MODPACKS/PARTS.BIN, read and checked once; 0: none (or not as tools/modpacks.py writes it, logged) */
static int load(void){
    if (g.loaded) return g.nparts > 0;
    g.loaded = 1;
    char path[1100];
    if (!fileio_mod_pack("PARTS.BIN", path, sizeof path)) return 0;
    FILE *f = fopen(path, "rb");
    if (!f){ fprintf(stderr, "[mods] MODPACKS/PARTS.BIN does not open: no mods' parts\n"); return 0; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 && n < (64L << 20) ? malloc((size_t)n) : 0;
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n){ fclose(f); free(b); fprintf(stderr, "[mods] MODPACKS/PARTS.BIN does not read\n"); return 0; }
    fclose(f);
    uint32_t nm = n >= (long)PK_HDR ? rd32(b + 8) : 0, np = n >= (long)PK_HDR ? rd32(b + 12) : 0;
    uint32_t nt = n >= (long)PK_HDR ? rd32(b + 16) : 0, ns = n >= (long)PK_HDR ? rd32(b + 20) : 0;
    uint64_t want = (uint64_t)PK_HDR + (uint64_t)PK_MOD * nm + (uint64_t)PK_PART * np + 8ull * nt + ns;
    if (n < (long)PK_HDR || memcmp(b, "MPRT", 4) || rd32(b + 4) != 1u || nm > MAX_MODS || np > MAX_PARTS || want != (uint64_t)n ||
        (ns && b[n - 1])){
        fprintf(stderr, "[mods] MODPACKS/PARTS.BIN is not a parts list as tools/modpacks.py writes it: no mods' parts\n");
        free(b); return 0;
    }
    const uint8_t *m = b + PK_HDR, *pp = m + PK_MOD * nm, *tt = pp + PK_PART * np;
    g.str = b + (want - ns); g.nstr = ns;
    g.p = calloc(np ? np : 1, sizeof *g.p);
    if (!g.p){ free(b); return 0; }
    g.nmods = (int)nm;
    for (uint32_t i = 0; i < nm; i++){
        memcpy(g.env[i], m + PK_MOD * i, 31); g.env[i][31] = 0;
        /* the rims are out for now (web/src/app/mods.ts): no shop offers them, whatever MODPACKS or
         * settings.ini says; back: g.on[i] = setting_bool(g.env[i], 0) */
        g.on[i] = 0;
    }
    for (uint32_t i = 0; i < np; i++){
        const uint8_t *q = pp + PK_PART * i;
        Part *p = &g.p[i];
        p->key = rd32(q); p->kind = q[4]; p->cl = q[5]; p->b6 = q[6]; p->mod = q[7];
        p->owner = rd32(q + 8); p->name = rd32(q + 12);
        p->m0 = q[16]; p->m1 = q[17]; p->mbase = rd32(q + 20);
        p->has_model = q[18] == 1;
        for (int k = 0; k < 8; k++) p->mstr[k] = rd32(q + 24 + 4 * k);
        p->nattr = rd32(q + 56);
        for (int k = 0; k < 8; k++){ p->attr[k][0] = rd32(q + 60 + 8 * k); p->attr[k][1] = rd32(q + 64 + 8 * k); }
        p->stand = rd32(q + 124);
        int bad = p->mod >= nm || p->nattr > 8 || !pstr(p->name) || p->kind == KIND_DECAL || p->kind == KIND_VINYL ||
                  (p->has_model && p->mbase != NONE && !pstr(p->mbase));
        for (int k = 0; k < 8 && !bad; k++) bad = p->mstr[k] != NONE && !pstr(p->mstr[k]);
        if (bad){
            fprintf(stderr, "[mods] MODPACKS/PARTS.BIN: part %u is not one this takes: no mods' parts\n", i);
            free(g.p); g.p = 0; free(b); return 0;
        }
    }
    g.nparts = (int)np;
    /* the brands, sorted by name, and their names' texts */
    for (int i = 0; i < g.nparts && g_nbrand < MAX_BRANDS; i++){
        if (g.p[i].kind != KIND_BRAND) continue;
        Brand *r = &g_brand[g_nbrand++];
        snprintf(r->name, sizeof r->name, "%s", pstr(g.p[i].name));
        r->key = g.p[i].key; r->mod = g.p[i].mod;
        char nm2[64];
        snprintf(nm2, sizeof nm2, "VISUAL_RIMS_BRAND_%s", r->name); r->tex = bhash(nm2);
        snprintf(nm2, sizeof nm2, "RIMS_BRAND_%s", r->name); r->text = bhash(nm2);
        const char *shown = r->name;
        for (uint32_t t = 0; t < nt; t++) if (rd32(tt + 8 * t) == r->text && pstr(rd32(tt + 8 * t + 4))) shown = pstr(rd32(tt + 8 * t + 4));
        language_add_text(nm2, shown);
    }
    for (int i = 1; i < g_nbrand; i++)
        for (int j = i; j > 0 && strcmp(g_brand[j - 1].name, g_brand[j].name) > 0; j--){ Brand x = g_brand[j]; g_brand[j] = g_brand[j - 1]; g_brand[j - 1] = x; }
    for (int i = 0; i < g.nmods; i++){
        int np2 = 0, nb = 0;
        for (int k = 0; k < g.nparts; k++) np2 += g.p[k].mod == i;
        for (int k = 0; k < g_nbrand; k++) nb += g_brand[k].mod == i;
        fprintf(stderr, "[mods] %s: %d parts (%d rim brand%s), %s in the shops\n", g.env[i], np2, nb, nb == 1 ? "" : "s",
                g.on[i] ? "on" : "off");
    }
    return g.nparts > 0;
}

/* the car part database's chunk with the packs' parts at its end, in guest memory for good; 0: as it is (logged why) */
uint32_t modpacks_grow(uint32_t chunk){
    if (!guest_ok_bytes(chunk, 16u) || MR32(chunk) != CHUNK_PARTS || !guest_ok_bytes(chunk, MR32(chunk + 4u) + 8u) || !load()) return 0;
    uint32_t size = MR32(chunk + 4u), at = chunk + 8u, end = chunk + 8u + size, sub[16][2];
    int n = 0;
    while (at + 8u <= end && n < 16){
        uint32_t sz = MR32(at + 4u);
        if (sz > end - at - 8u) break;
        sub[n][0] = at; sub[n][1] = sz; n++;
        at += 8u + sz;
    }
    int laid = n >= 7 && at == end;
    for (int k = 0; laid && k < 7; k++) laid = MR32(sub[k][0]) == SUB[k];
    if (!laid){ fprintf(stderr, "[mods] the car part database is laid out unlike the game's: no mods' parts\n"); return 0; }
    uint32_t hdr = sub[S_HDR][0] + 8u, prt = sub[S_PART][0] + 8u, own = sub[S_OWNER][0] + 8u, str = sub[S_STR][0] + 8u;
    uint32_t nattr = MR32(hdr + 0x20u), nown = MR32(hdr + 0x28u), nmodel = MR32(hdr + 0x30u), nparts = MR32(hdr + 0x38u);
    if (sub[S_HDR][1] < 0x3cu || nattr * 8u > sub[S_ATTR][1] || nparts * PART_SZ > sub[S_PART][1] || nown * 4u > sub[S_OWNER][1] ||
        nmodel * MODEL_SZ > sub[S_MODEL][1] || sub[S_STR][1] & 3u || nown > 256u){
        fprintf(stderr, "[mods] the car part database's counts are not its tables': no mods' parts\n"); return 0;
    }
    /* the game's brands (for the brand list's order), and no key of ours one of its parts' */
    g_ngbrand = 0;
    for (uint32_t i = 0; i < nparts; i++){
        uint32_t p = prt + i * PART_SZ, key = MR32(p);
        for (int k = 0; k < g.nparts; k++)
            if (g.p[k].key == key && MR8(p + 4u) == g.p[k].kind){
                fprintf(stderr, "[mods] the car part database has a part of the mods' packs already (%08x): no mods' parts\n", key);
                return 0;
            }
        if (MR8(p + 4u) == KIND_BRAND && g_ngbrand < 64){
            uint32_t s = str + 4u * MR16(p + 8u);
            g_gbrand[g_ngbrand].key = key;
            int j = 0;
            for (; j < 23 && s + (uint32_t)j < str + sub[S_STR][1] && MR8(s + (uint32_t)j); j++) g_gbrand[g_ngbrand].name[j] = (char)MR8(s + (uint32_t)j);
            g_gbrand[g_ngbrand].name[j] = 0;
            g_ngbrand++;
        }
    }
    /* the owners the parts need that the table lacks */
    uint32_t newown[16]; int nnew = 0;
    for (int k = 0; k < g.nparts; k++){
        int have = 0;
        for (uint32_t i = 0; i < nown && !have; i++) have = MR32(own + 4u * i) == g.p[k].owner;
        for (int i = 0; i < nnew && !have; i++) have = newown[i] == g.p[k].owner;
        if (have) continue;
        if (nnew == 16 || nown + (uint32_t)nnew >= 256u){ fprintf(stderr, "[mods] too many owners: no mods' parts\n"); return 0; }
        newown[nnew++] = g.p[k].owner;
    }
    /* what each table gains, kept to its alignment: strings 4 (the next table 8), lists 8, models and owners 8 */
    uint32_t g_str = 0, g_lst = 0, g_atr = 0, g_mdl = 0;
    uint32_t *soff = calloc(g.nstr + 1u, sizeof *soff);         /* a pack string's index in the table, +1 (0: not yet) */
    if (!soff) return 0;
    #define USE_STR(o) do { if ((o) != NONE && !soff[(o)]){ soff[(o)] = 1u; g_str += ((uint32_t)strlen(pstr(o)) + 4u) & ~3u; } } while (0)
    for (int k = 0; k < g.nparts; k++){
        Part *p = &g.p[k];
        USE_STR(p->name);
        if (p->has_model){ USE_STR(p->mbase); for (int j = 0; j < 8; j++) USE_STR(p->mstr[j]); g_mdl += MODEL_SZ; }
        if (p->nattr){ g_lst += 2u + 2u * p->nattr; g_atr += 8u * p->nattr; }
    }
    #undef USE_STR
    g_str = (g_str + 7u) & ~7u; g_lst = (g_lst + 7u) & ~7u; g_mdl = (g_mdl + 7u) & ~7u;
    uint32_t g_own = ((uint32_t)nnew * 4u + 7u) & ~7u, g_prt = (uint32_t)g.nparts * PART_SZ;
    /* the tables' slack past their counts (the game's has none but alignment) moves after the new rows */
    uint32_t mslack = sub[S_MODEL][1] - nmodel * MODEL_SZ, oslack = sub[S_OWNER][1] - nown * 4u, pslack = sub[S_PART][1] - nparts * PART_SZ;
    uint32_t gain[7] = { 0, g_str, g_lst, g_atr, g_mdl, g_own, g_prt }, nsize = size;
    for (int k = 0; k < 7; k++) nsize += gain[k];
    if ((sub[S_STR][1] + g_str) / 4u > 0xffffu || (sub[S_LIST][1] + g_lst) / 2u > 0xffffu || nattr + g_atr / 8u > 0x7fffu ||
        nmodel + (uint32_t)g.nparts > 0xffffu){
        fprintf(stderr, "[mods] the car part database's tables are too full: no mods' parts\n"); free(soff); return 0;
    }
    uint32_t out = gheap_alloc(1, nsize + 8u + 16u, 1);
    if (!out){ fprintf(stderr, "[mods] no memory for the car part database's copy\n"); free(soff); return 0; }
    out = (out + 15u) & ~15u;
    MR32(out) = CHUNK_PARTS; MR32(out + 4u) = nsize;
    uint32_t o = out + 8u, place[7];
    for (int k = 0; k < n; k++){
        uint32_t sz = sub[k][1] + (k < 7 ? gain[k] : 0u);
        MR32(o) = MR32(sub[k][0]); MR32(o + 4u) = sz;
        memcpy(&MR8(o + 8u), &MR8(sub[k][0] + 8u), sub[k][1]);
        if (k < 7) place[k] = o + 8u;
        o += 8u + sz;
    }
    /* the rows after each table's count: attributes, models, owners, parts (their slack moves after them) */
    uint32_t a_at = place[S_ATTR] + nattr * 8u, m_at = place[S_MODEL] + nmodel * MODEL_SZ;
    uint32_t w_at = place[S_OWNER] + nown * 4u, p_at = place[S_PART] + nparts * PART_SZ;
    memmove(&MR8(a_at + g_atr), &MR8(a_at), sub[S_ATTR][1] - nattr * 8u);         memset(&MR8(a_at), 0, g_atr);
    memmove(&MR8(m_at + g_mdl), &MR8(m_at), mslack);                              memset(&MR8(m_at), 0, g_mdl);
    memmove(&MR8(w_at + g_own), &MR8(w_at), oslack);                              memset(&MR8(w_at), 0, g_own);
    memmove(&MR8(p_at + g_prt), &MR8(p_at), pslack);                              memset(&MR8(p_at), 0, g_prt);
    /* strings: each used one once, after the game's */
    uint32_t s_at = place[S_STR] + sub[S_STR][1];
    for (uint32_t i = 0; i < g.nstr; i++){
        if (!soff[i]) continue;
        const char *t = pstr(i);
        size_t l = strlen(t);
        soff[i] = (s_at - place[S_STR]) / 4u + 1u;
        memcpy(&MR8(s_at), t, l + 1u); s_at += ((uint32_t)l + 4u) & ~3u;
    }
    #define SIX(o) ((o) == NONE ? NONE : soff[(o)] - 1u)
    for (int i = 0; i < nnew; i++) MR32(w_at + 4u * (uint32_t)i) = newown[i];
    uint32_t l_at = place[S_LIST] + sub[S_LIST][1], ai = nattr, mi = nmodel;
    for (int k = 0; k < g.nparts; k++){
        Part *p = &g.p[k];
        uint32_t wi = 0;
        for (uint32_t i = 0; i < nown + (uint32_t)nnew; i++) if (MR32(place[S_OWNER] + 4u * i) == p->owner){ wi = i; break; }
        uint32_t li = 0xffffu, model = 0xffffu;
        if (p->nattr){
            li = (l_at - place[S_LIST]) / 2u;
            MR16(l_at) = (uint16_t)p->nattr;
            for (uint32_t j = 0; j < p->nattr; j++){
                MR32(a_at) = p->attr[j][0]; MR32(a_at + 4u) = p->attr[j][1]; a_at += 8u;
                MR16(l_at + 2u + 2u * j) = (uint16_t)ai++;
            }
            l_at += 2u + 2u * p->nattr;
        }
        if (p->has_model){
            MR8(m_at) = p->m0; MR8(m_at + 1u) = p->m1;
            MR16(m_at + 2u) = (uint16_t)(p->mbase == NONE ? 0xffffu : SIX(p->mbase));
            for (int j = 0; j < 8; j++) MR32(m_at + 4u + 4u * (uint32_t)j) = SIX(p->mstr[j]);
            m_at += MODEL_SZ; model = (uint16_t)mi++;
        }
        MR32(p_at) = p->key; MR8(p_at + 4u) = p->kind; MR8(p_at + 5u) = p->cl; MR8(p_at + 6u) = p->b6; MR8(p_at + 7u) = (uint8_t)wi;
        MR16(p_at + 8u) = (uint16_t)SIX(p->name); MR16(p_at + 0xau) = (uint16_t)li; MR16(p_at + 0xcu) = (uint16_t)model;
        p_at += PART_SZ;
    }
    #undef SIX
    free(soff);
    MR32(place[S_HDR] + 0x20u) = ai;
    MR32(place[S_HDR] + 0x28u) = nown + (uint32_t)nnew;
    MR32(place[S_HDR] + 0x30u) = mi;
    MR32(place[S_HDR] + 0x38u) = nparts + (uint32_t)g.nparts;
    g_grown = 1;
    fprintf(stderr, "[mods] the car part database: %u parts, %d added from MODPACKS (%d rim brand%s)\n",
            nparts + (uint32_t)g.nparts, g.nparts, g_nbrand, g_nbrand == 1 ? "" : "s");
    return out;
}

/* 0x5463B0 (this, brand key, icon, name text), thiscall, ret 0xC: with a brand of ours */
static void add_brand(CPU *c, const Brand *b){
    CPU save = *c;
    c->esp = (c->esp - 256u) & ~15u;
    PUSH32(b->text); PUSH32(b->tex); PUSH32(b->key);
    PUSH32(MR32(save.esp));
    c->ecx = save.ecx;
    sub_005463b0_orig(c);
    *c = save;
}
static const char *game_brand(uint32_t key){
    for (int i = 0; i < g_ngbrand; i++) if (g_gbrand[i].key == key) return g_gbrand[i].name;
    return 0;
}
void sub_005463b0(CPU *c){
    uint32_t ret = MR32(c->esp), key = MR32(c->esp + 4u);
    if (ret == RET_LIST) g_cur = 0;
    else if (ret == RET_LIST_SUV) g_cur = -1;            /* an SUV's list: the game's few brands only */
    if (g_cur >= 0 && g_nbrand && g_grown){
        const char *nm = key == KEY_STOCK ? 0 : game_brand(key);
        while (g_cur < g_nbrand && (key == KEY_STOCK || (nm && strcasecmp(g_brand[g_cur].name, nm) < 0))){
            const Brand *b = &g_brand[g_cur++];
            if (!g.on[b->mod]) continue;
            add_brand(c, b);
            static int said;
            if (said++ < 8) fprintf(stderr, "[mods] the rim shop lists %s (%s)\n", b->name, g.env[b->mod]);
        }
        if (key == KEY_STOCK) g_cur = -1;
    }
    sub_005463b0_orig(c);
}

/* 0x50E960 (brand key) -> its name, thiscall, ret 4 */
void sub_0050e960(CPU *c){
    uint32_t key = MR32(c->esp + 4u);
    for (int i = 0; g_grown && i < g_nbrand; i++){
        if (g_brand[i].key != key) continue;
        uint32_t t = language_text(g_brand[i].text);
        if (!t) break;
        c->eax = t;
        POP32(c->eip); c->esp += 4u;
        return;
    }
    sub_0050e960_orig(c);
}

/* --- a profile's mod parts: MODPARTS.DAT beside it ----------------------------
 * Our saves work in the original game, which has no mod parts.  So a profile
 * (and garage.c's GARAGE.DAT, which a page of before reads) holds the game's
 * parts only: while the game makes a profile's payload (0x52A280, garage.c),
 * each car slot with a mod's part holds a part of the game's (the stand-in:
 * the mod part's look-alike, tools/modpacks.py, else the slot's stock part,
 * what 0x637040 gives a new ride of that car), and the mod parts go into
 * "NFS Underground 2/<NAME>/MODPARTS.DAT".  When a profile's cars load
 * (0x516850, garage.c), each goes back on its car where that car is still as
 * it was saved: found by its handle, of the same car type, the stand-in in
 * the slot and its parts as saved (a fingerprint of its 170 keys, the
 * stand-ins among them).  A car changed elsewhere (the original game, a page
 * of before) keeps what it has.  A part this game lacks (no MODPACKS) waits
 * and is written again with that profile's next save while its car keeps the
 * stand-in.  The original game, a page of before and a game without MODPACKS
 * see the stand-ins: the stock parts.  The game's "anything to save?"
 * (0x52A4C0) counts a change of them.  The same for the Japanese release's
 * vinyls, Art Factory 28-34 (vinylpacks.c, VINYLPACKS): in no profile, the
 * slot's stock part there (the original game draws no vinyl for a key it
 * lacks, and its next change to the car saves none); a profile saved before
 * holds their keys, which load as they are and go out at its next save.
 *
 * A car of a car type this game has not (a modded game's, with added cars)
 * is the stock car of the game's first type here (garage.c addon_cars: the
 * game reads such a car type's info through a null pointer, 0x503880), and
 * stays in its profile as it is: g_add keeps its own car type and 170 keys,
 * and a payload made while its record still holds that stand-in untouched
 * (the bytes of its parts, tuning and money as made) has the car as it was
 * loaded.  A car the player changed here (parts, paint, tuning: other bytes),
 * traded or replaced (garage.c: Trade Car hands it over to the record it
 * moves into; the car lot's buy and trade-in and a new tuned car, 0x516B90
 * and 0x52A710, and a car no more in use drop it) is saved as it now is.
 * Nothing of it is beside the profile: a page or game that has its car type
 * reads it from the profile itself.
 *
 * The order of a save (garage.c 0x52A450): the payload with the stand-ins,
 * then MODPARTS.NEW, then the profile; only once the profile is written
 * MODPARTS.NEW becomes MODPARTS.DAT.  When MODPARTS.NEW cannot be written
 * (a full disk: a new file fails where overwriting works), the payload is
 * made again with every part as it is, the waiting ones in their slots too:
 * the profile then holds what a page of before would have, and needs
 * nothing beside it.  A file's head has a counter of its writes (and its
 * first entry, of kind 3, the counter and its profile's name), so a load
 * takes the newer of MODPARTS.DAT and a MODPARTS.NEW of the same profile (a
 * page closed between the profile and the rename: the browser's rename
 * takes the old file away first) and finishes that rename.
 *
 * The cars with parts a player can change: the table's 20 Customize cars
 * (+0x544, 0x434 bytes each) and the career's 5 (0x7F8), garage.c's slots
 * 6-10 (garage_records; the table's 48 stock and 12 other records are 0x1C
 * bytes, its 6 presets the game's own).  Their parts at +0x18: +8 the car
 * type, +0xC + 4 x slot each of the 170 slots' part key (0: none), as 0x503880
 * makes a ride of them and 0x503950 writes a ride back.
 *
 * MODPARTS.DAT: 'N2MP', version 1, count, its write counter (0 from a page of
 * before), then count entries of 8 words: {handle, car type, slot, the mod
 * part's key, the stand-in's key, the car's fingerprint (2 words), kind};
 * kind 0 a slot's part, kind 3 the head {'MPHD', bStringHash of the
 * profile's name in capitals, the write counter, 0 ..., 3}, kind 4 a car's
 * stance (stance.c: {handle, car type, its camber, its offset, its tires'
 * width, the car's fingerprint (2 words), 4}: taken only for a car as it was
 * saved, as a part; the car as the game draws it where there is none) (a page of before
 * keeps it as another kind; one there with a head word 0 is that page's
 * write: the newest).  An entry of another kind (a later page's) is kept as
 * it is and written again; a file this page cannot read (another version,
 * cut short) is never written over or removed: a save with mod parts of its
 * own first keeps it aside as MODPARTS.OLD (or .OLD2 ... .OLD9 beside an
 * older one; all taken: it stays, and ours stays MODPARTS.NEW). */
int garage_records(uint32_t db, uint32_t *out, int max);
int garage_car_in_use(uint32_t db, uint32_t r);
int fileio_resolve_save(const char *win, char *out, size_t outsz);
int fileio_resolve_save_w(const char *win, char *out, size_t outsz);    /* (a file to write there) */
int vinylpacks_key(uint32_t key);                       /* vinylpacks.c: one of Art Factory 28-34's */
int vinylpacks_have(void);                              /* ... the car part database has them */

#define MP_MAGIC      0x504d324eu   /* "N2MP" */
#define MP_FILE       "MODPARTS.DAT"
#define MP_NEW        "MODPARTS.NEW"
#define MP_OLD        "MODPARTS.OLD"
#define MP_OLDS       9             /* .OLD, .OLD2 ... .OLD9 */
#define MP_MAX        1024
#define R_HANDLE      0x08u
#define R_PARTS       0x18u         /* +8 car type, +0xC + 4 x slot: the keys */
#define N_SLOTS       0xaau
#define FN_TYPE_INFO  0x00610130u   /* cdecl (car type) -> its info (+0x840: what a ride's Init takes) */
#define FN_RIDE_STOCK 0x00637040u   /* thiscall ride (0): every slot its stock part */
#define RIDE_PARTS    0x590u        /* a ride's parts: +4 x slot */
#define CARS0_OF(p)   (G_FECARS + (uint32_t)(p) * 0xab4cu)   /* a player's cars (garage.c's G_FECARS, CARS_SZ) */
#define K_PART        0u            /* an entry's kind: a slot's part */
#define K_HEAD        3u            /* ... the file's head: its profile, its write counter */
#define K_STANCE      STANCE_KIND   /* ... a car's stance (stance.c) */
#define HEAD_MAGIC    0x4448504du   /* "MPHD" */
#define ADDON_MAX     40            /* (20 Customize cars, 10 slots, 6 presets a player) */
#define CARTYPE_SZ    0x890u
#define CARTYPES_N    46u           /* 0x610130 looks a car type up among these */
#define KEY_AT(r, s)  ((r) + R_PARTS + 0xcu + 4u * (s))

typedef struct { uint32_t w[8]; uint8_t wait; } MPart;  /* w: as the file has it; wait: one of g_wait's */
#define E_HANDLE(e)   ((e)->w[0])
#define E_TYPE(e)     ((e)->w[1])
#define E_SLOT(e)     ((e)->w[2])
#define E_KEY(e)      ((e)->w[3])
#define E_STAND(e)    ((e)->w[4])
#define E_FP(e)       ((uint64_t)(e)->w[5] | (uint64_t)(e)->w[6] << 32)
#define E_KIND(e)     ((e)->w[7])
typedef struct { MPart e[MP_MAX]; int n; } MList;
/* a car of a car type this game has not, a stand-in here: its record (size bytes), its own car type and keys, the
 * stand-in's bytes as made (base, from +0x18 on), whether it was a car (in use) then */
typedef struct { uint32_t r, size, orig, keys[N_SLOTS]; uint64_t base; int used; } Addon;
static MList g_wait[2];                                  /* each player's parts not here (no MODPACKS), and other kinds */
static char g_wait_name[2][33];                          /* ... of that profile */
static struct { char name[33]; uint32_t count; } g_cnt[8]; static int g_ncnt;   /* each profile's write counter */
static struct { Addon a[ADDON_MAX]; int n; } g_add[2];   /* each player's cars of car types this game has not */
static MList g_cur_l;                                    /* the payload being made */
static struct { uint32_t at, key; } g_hid[MP_MAX * 2]; static int g_nhid;
static struct { uint32_t r, type, keys[N_SLOTS]; } g_unc[2 * ADDON_MAX]; static int g_nunc;   /* stand-ins, meanwhile */
static struct { uint32_t at, key; } g_wraw[MP_MAX]; static int g_nwraw;                     /* waiting slots, meanwhile */
static struct { char name[33]; uint64_t fp; MList l; unsigned seq; } g_mb[4];   /* the payloads made, newest kept */
static unsigned g_mseq;
static struct { int on, p; uint64_t fp; MList l; } g_mk;                        /* the one the game keeps */
static struct { int on, empty; char name[33]; uint64_t fp; uint32_t count; } g_staged;   /* MODPARTS.NEW of a save */
static char g_foreign[8][33]; static int g_nforeign;    /* profiles whose MODPARTS.DAT this page cannot read */
static uint32_t g_ride;                                  /* a ride of ours, for the stock parts */
static struct { uint32_t type, key[N_SLOTS]; } g_stock[16]; static int g_nstock;

static int mod_key(uint32_t key){
    if (!key || !g_grown) return 0;
    for (int i = 0; i < g.nparts; i++) if (g.p[i].key == key) return 1;
    return 0;
}
/* a part kept beside the profile, never in it: a mod's (with MODPACKS) or one of Art Factory 28-34 (always) */
static int aside_key(uint32_t key){ return mod_key(key) || vinylpacks_key(key); }
/* ... and one this game has, to put back */
static int have_key(uint32_t key){ return mod_key(key) || (vinylpacks_key(key) && vinylpacks_have()); }
/* a car type's info (0x610130's search, without the call): its CarTypeInfo, 0 none; -1 not known yet (no car types) */
int64_t modparts_cartype(uint32_t hash){
    uint32_t t = MR32(G_CARTYPES);
    if (!guest_ok_bytes(t, CARTYPES_N * CARTYPE_SZ)) return -1;
    for (uint32_t i = 0; hash && i < CARTYPES_N; i++) if (MR32(t + i * CARTYPE_SZ + 0xd0u) == hash) return t + i * CARTYPE_SZ;
    return 0;
}
/* a mod part's look-alike among the game's (tools/modpacks.py LOOKALIKES: a rim of the same size), 0: none */
static uint32_t lookalike(uint32_t key){
    for (int i = 0; i < g.nparts; i++) if (g.p[i].key == key) return g.p[i].stand;
    return 0;
}
/* the stock part's key of a car type's slot (0: none, or not known: then 0 all the same, the game's "no part") */
static uint32_t stock_key(CPU *c, uint32_t type, uint32_t slot){
    for (int i = 0; i < g_nstock; i++) if (g_stock[i].type == type) return g_stock[i].key[slot];
    if (!g_ride && !(g_ride = gheap_alloc(1, 0x1000u, 1))) return 0;
    uint32_t info = gcall(c, FN_TYPE_INFO, 0, 1, &type);
    int i = g_nstock < 16 ? g_nstock++ : 15;
    g_stock[i].type = type;
    memset(g_stock[i].key, 0, sizeof g_stock[i].key);
    if (!info || !mem_is_committed(info + 0x840u)){ fprintf(stderr, "[mods] car type %08x: no info for its stock parts\n", type); return 0; }
    for (uint32_t o = 0; o < 0x1000u; o += 4u) MR32(g_ride + o) = 0;
    uint32_t a[4] = { MR32(info + 0x840u), 1u, 0, 0 }, z = 0;
    gcall(c, FN_RIDE_INIT, g_ride, 4, a);
    gcall(c, FN_RIDE_STOCK, g_ride, 1, &z);
    for (uint32_t s = 0; s < N_SLOTS; s++){
        uint32_t part = MR32(g_ride + RIDE_PARTS + 4u * s);
        g_stock[i].key[s] = part && mem_is_committed(part + 3u) ? MR32(part) : 0;
    }
    return g_stock[i].key[slot];
}
/* a part kept beside the profile: what the profile has in its slot */
static uint32_t stand_in(CPU *c, uint32_t type, uint32_t slot, uint32_t key){
    uint32_t la = lookalike(key);
    return la ? la : stock_key(c, type, slot);
}
static uint64_t car_fp(uint32_t r){
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t o = 8u; o < 0xcu + 4u * N_SLOTS; o++){ h ^= MR8(r + R_PARTS + o); h *= 0x100000001b3ull; }
    return h;
}
/* a record's bytes from +0x18 to its end: its parts, tuning, performance, the money spent on it */
static uint64_t rec_fp(uint32_t r, uint32_t size){
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t o = R_PARTS; o < size; o++){ h ^= MR8(r + o); h *= 0x100000001b3ull; }
    return h;
}
/* garage.c: a car type's stock keys, all 170 (0 where the slot has none) */
void modparts_stock_keys(CPU *c, uint32_t type, uint32_t *keys){
    for (uint32_t s = 0; s < N_SLOTS; s++) keys[s] = stock_key(c, type, s);
}
static uint32_t car_of(uint32_t db, uint32_t handle, uint32_t type){
    uint32_t r[64]; int n = garage_records(db, r, 64);
    for (int i = 0; i < n; i++) if (MR32(r[i] + R_HANDLE) == handle && MR32(r[i] + R_PARTS + 8u) == type) return r[i];
    return 0;
}
/* two entries for "anything to save?": a part by its car, slot and key; a stance by its car, car type and values (its car's
 * fingerprint aside: a payload's has the stand-ins in, the cars now the mods' parts); an entry of another kind whole */
static int entry_differs(const MPart *a, const MPart *b){
    if (E_KIND(a) != E_KIND(b) || E_HANDLE(a) != E_HANDLE(b) || E_SLOT(a) != E_SLOT(b) || E_KEY(a) != E_KEY(b)) return 1;
    if (E_KIND(a) == K_STANCE) return a->w[1] != b->w[1] || a->w[4] != b->w[4];
    return E_KIND(a) != K_PART && memcmp(a->w, b->w, sizeof a->w);
}
static int lists_differ(const MList *a, const MList *b){
    if (a->n != b->n) return 1;
    for (int i = 0; i < a->n; i++) if (entry_differs(&a->e[i], &b->e[i])) return 1;
    return 0;
}
static int is_foreign(const char *name){
    for (int i = 0; i < g_nforeign; i++) if (!strcasecmp(g_foreign[i], name)) return i;
    return -1;
}
/* a profile's name its folder can have (the game's field, longer ones of the PC game's, bytes of its fonts): not empty,
 * no path's separators, no control characters, not "." or ".." */
static int name_ok(const char *s){
    if (!s[0] || !strcmp(s, ".") || !strcmp(s, "..")) return 0;
    for (; *s; s++) if ((uint8_t)*s < 0x20u || *s == '/' || *s == '\\' || *s == ':' || *s == '*' || *s == '?' || *s == '"' ||
                        *s == '<' || *s == '>' || *s == '|') return 0;
    return 1;
}
/* the head's name: bStringHash of the name in capitals (a folder's name in any case is the same profile's) */
static uint32_t name_hash(const char *s){
    uint32_t h = 0xffffffffu;
    for (; *s; s++) h = h * 33u + (uint8_t)((*s >= 'a' && *s <= 'z') ? *s - 32 : *s);
    return h;
}

/* --- the cars of car types this game has not: as they were, in the profile */
static int addon_of(int p, uint32_t r){
    for (int i = 0; i < g_add[p].n; i++) if (g_add[p].a[i].r == r) return i;
    return -1;
}
static void addon_drop(int p, int i){ g_add[p].a[i] = g_add[p].a[--g_add[p].n]; }
/* garage.c: the record r of player p's cars (db; size bytes) was a car (used: in use) of the car type orig, which this
 * game has not, with these keys (all 170); it is the stand-in now: in each payload as it was while it stays so */
void modparts_track(int p, uint32_t r, uint32_t size, uint32_t orig, const uint32_t *keys, int used){
    if (p < 0 || p > 1 || addon_of(p, r) >= 0) return;
    if (g_add[p].n == ADDON_MAX){ fprintf(stderr, "[mods] more than %d cars of car types this game has not: car type 0x%08x saved as the stand-in\n", ADDON_MAX, orig); return; }
    Addon *a = &g_add[p].a[g_add[p].n++];
    memset(a, 0, sizeof *a);
    a->r = r; a->size = size; a->orig = orig; a->used = used;
    memcpy(a->keys, keys, sizeof a->keys);
    a->base = rec_fp(r, size);
}
/* the cars as they were for a payload (their stand-ins kept meanwhile, the stand-ins after: reconvert); a car the
 * player changed here (its bytes not the stand-in's as made, or no more in use) is the player's own now */
static void unconvert(uint32_t db, int p){
    for (int i = 0; i < g_add[p].n; i++){
        Addon *a = &g_add[p].a[i];
        if (rec_fp(a->r, a->size) != a->base || (a->used && !garage_car_in_use(db, a->r))){
            fprintf(stderr, "[mods] the car 0x%08x of car type 0x%08x was changed here: saved as it now is\n", MR32(a->r + R_HANDLE), a->orig);
            addon_drop(p, i--);
            continue;
        }
        if (g_nunc == 2 * ADDON_MAX) continue;
        g_unc[g_nunc].r = a->r; g_unc[g_nunc].type = MR32(a->r + R_PARTS + 8u);
        for (uint32_t s = 0; s < N_SLOTS; s++) g_unc[g_nunc].keys[s] = MR32(KEY_AT(a->r, s));
        MR32(a->r + R_PARTS + 8u) = a->orig;
        for (uint32_t s = 0; s < N_SLOTS; s++) MR32(KEY_AT(a->r, s)) = a->keys[s];
        g_nunc++;
    }
}
/* whether r is a car as it was, for the payload being made (so its keys are as the profile had them, all 170) */
static int unconverted(uint32_t r){
    for (int i = 0; i < g_nunc; i++) if (g_unc[i].r == r) return 1;
    return 0;
}
static void reconvert(void){
    for (int i = g_nunc - 1; i >= 0; i--){
        MR32(g_unc[i].r + R_PARTS + 8u) = g_unc[i].type;
        for (uint32_t s = 0; s < N_SLOTS; s++) MR32(KEY_AT(g_unc[i].r, s)) = g_unc[i].keys[s];
    }
    g_nunc = 0;
}
/* stance.c: the car type player p's record r is saved with (a car of a car type this game has not: its own, not the
 * stand-in's it has here) */
uint32_t modparts_car_type(int p, uint32_t r){
    int i = p >= 0 && p <= 1 ? addon_of(p, r) : -1;
    return i >= 0 ? g_add[p].a[i].orig : MR32(r + R_PARTS + 8u);
}
/* garage.c: player p's car of the record r (its handle h) has left it (traded; a new car in its place, 0x516B90,
 * 0x52A710): nothing kept for it any more */
void modparts_car_gone(int p, uint32_t r, uint32_t h){
    if (p < 0 || p > 1) return;
    int i = addon_of(p, r), k = 0, n = 0;
    if (i >= 0){ fprintf(stderr, "[mods] the car 0x%08x of car type 0x%08x has gone: saved as the record now is\n", h, g_add[p].a[i].orig); addon_drop(p, i); }
    for (int j = 0; h && j < g_wait[p].n; j++){
        uint32_t kd = E_KIND(&g_wait[p].e[j]);
        if ((kd == K_PART || kd == K_STANCE) && E_HANDLE(&g_wait[p].e[j]) == h){ n++; continue; }
        g_wait[p].e[k++] = g_wait[p].e[j];
    }
    if (h) g_wait[p].n = k;
    if (n) fprintf(stderr, "[mods] the car 0x%08x has gone: its %d waiting entr%s (parts, stance) not kept any more\n", h, n, n == 1 ? "y" : "ies");
    stance_car_gone(p, h);
}
/* garage.c (Trade Car): player p's car of the record from (its handle hf) is now in the record to (handle ht), as it
 * was, byte for byte */
void modparts_car_moved(int p, uint32_t from, uint32_t hf, uint32_t to, uint32_t ht){
    if (p < 0 || p > 1 || from == to) return;
    modparts_car_gone(p, to, 0);
    int i = addon_of(p, from);
    if (i >= 0) g_add[p].a[i].r = to;
    for (int j = 0; j < g_wait[p].n; j++)
        if ((E_KIND(&g_wait[p].e[j]) == K_PART || E_KIND(&g_wait[p].e[j]) == K_STANCE) && E_HANDLE(&g_wait[p].e[j]) == hf) E_HANDLE(&g_wait[p].e[j]) = ht;
    stance_car_moved(p, hf, ht);
}

/* --- the parts kept beside the profile */
/* the cars' mod parts now (hide: the stand-ins put in for the while; every mod part goes out of the cars even when
 * the list is full; a car of a car type this game has not, as it was for the payload, keeps all its keys), the
 * waiting ones with them */
static void collect(CPU *c, uint32_t db, int p, MList *l, int hide){
    uint32_t r[64]; int n = garage_records(db, r, 64), full = 0;
    l->n = 0;
    if (hide) g_nhid = 0;
    for (int i = 0; i < n; i++){
        uint32_t handle = MR32(r[i] + R_HANDLE), type = MR32(r[i] + R_PARTS + 8u);
        if (unconverted(r[i])) continue;                 /* (a modded game's car as it was: its own keys, a mod's too) */
        for (uint32_t s = 0; s < N_SLOTS; s++){
            uint32_t at = KEY_AT(r[i], s), key = MR32(at);
            if (!aside_key(key)) continue;
            uint32_t stand = stand_in(c, type, s, key);
            if (l->n < MP_MAX){
                MPart *e = &l->e[l->n++];
                memset(e, 0, sizeof *e);
                e->w[0] = handle; e->w[1] = type; e->w[2] = s; e->w[3] = key; e->w[4] = stand;
            } else full++;
            if (hide && g_nhid < MP_MAX * 2){ g_hid[g_nhid].at = at; g_hid[g_nhid].key = key; g_nhid++; MR32(at) = stand; }
        }
    }
    if (full) fprintf(stderr, "[mods] %d mod part%s more than MODPARTS.DAT holds: the stock ones in the profile, not kept\n", full, full == 1 ? "" : "s");
    {   /* the cars' stances (each one's record there: stance.c keeps them by its handle and car type) */
        uint32_t st[STANCE_CARS][8];
        int ns = stance_entries(p, st, STANCE_CARS);
        for (int i = 0; i < ns && l->n < MP_MAX; i++){
            if (!car_of(db, st[i][0], st[i][1])) continue;
            MPart *e = &l->e[l->n++]; memset(e, 0, sizeof *e); memcpy(e->w, st[i], sizeof e->w);
        }
    }
    for (int i = 0; i < g_wait[p].n && l->n < MP_MAX; i++){
        const MPart *w = &g_wait[p].e[i];
        if (E_KIND(w) == K_HEAD) continue;                                         /* (each file its own) */
        if (E_KIND(w) == K_STANCE && stance_has(p, E_HANDLE(w))) continue;         /* (a stance set here since: that one) */
        if (E_KIND(w)){ l->e[l->n] = *w; l->e[l->n++].wait = 1; continue; }        /* another kind: as it was */
        uint32_t car = car_of(db, E_HANDLE(w), E_TYPE(w));
        if (car && MR32(KEY_AT(car, E_SLOT(w))) == E_STAND(w)){ l->e[l->n] = *w; l->e[l->n++].wait = 1; }
    }
    for (int i = 0; i < l->n; i++){
        if (E_KIND(&l->e[i]) != K_PART && (E_KIND(&l->e[i]) != K_STANCE || l->e[i].wait)) continue;   /* (a waiting one: as it was) */
        uint32_t car = car_of(db, E_HANDLE(&l->e[i]), E_TYPE(&l->e[i]));
        uint64_t fp = car ? car_fp(car) : 0;
        l->e[i].w[5] = (uint32_t)fp; l->e[i].w[6] = (uint32_t)(fp >> 32);
    }
}
/* the waiting ones are that profile's alone */
static void own_only(MList *l, int p, const char *name){
    if (!strcasecmp(name, g_wait_name[p])) return;
    int k = 0;
    for (int i = 0; i < l->n; i++) if (!l->e[i].wait) l->e[k++] = l->e[i];
    l->n = k;
}
/* garage.c: a payload is being made of player p's cars (db): the cars of car types this game has not as they were,
 * the mod parts' stand-ins */
void modparts_hide(CPU *c, uint32_t db, int p){
    if (p < 0 || p > 1) return;
    unconvert(db, p);
    collect(c, db, p, &g_cur_l, 1);
}
/* ... the cars as they were */
void modparts_show(void){
    for (int i = 0; i < g_nhid; i++) MR32(g_hid[i].at) = g_hid[i].key;
    g_nhid = 0;
    for (int i = 0; i < g_nwraw; i++) MR32(g_wraw[i].at) = g_wraw[i].key;
    g_nwraw = 0;
    reconvert();
}
/* garage.c, a save whose MODPARTS.NEW could not be written: on, the payload to be made again as a page of before has
 * it: the cars of car types this game has not as they were, every mod part as it is, each waiting one in its slot
 * (its car still the stand-in's: the waiting part's own); off, all as before (modparts_show) */
void modparts_raw(CPU *c, uint32_t db, int p, int on){
    (void)c;
    if (p < 0 || p > 1) return;
    if (!on){ modparts_show(); return; }
    unconvert(db, p);
    g_nwraw = 0;
    for (int i = 0; i < g_wait[p].n && g_nwraw < MP_MAX; i++){
        const MPart *w = &g_wait[p].e[i];
        if (E_KIND(w)) continue;
        uint32_t car = car_of(db, E_HANDLE(w), E_TYPE(w));
        if (!car || MR32(KEY_AT(car, E_SLOT(w))) != E_STAND(w)) continue;
        g_wraw[g_nwraw].at = KEY_AT(car, E_SLOT(w)); g_wraw[g_nwraw].key = E_STAND(w); g_nwraw++;
        MR32(KEY_AT(car, E_SLOT(w))) = E_KEY(w);
    }
}
/* ... and was: its mod parts kept with it (by its cars' fingerprint and the profile's name) */
void modparts_built(int p, uint32_t out, uint64_t fp, const char *name, int kept){
    (void)out;
    modparts_show();
    if (p < 0 || p > 1 || !fp) return;
    own_only(&g_cur_l, p, name);
    unsigned at = 0;
    for (unsigned i = 1; i < 4u; i++) if (g_mb[i].seq < g_mb[at].seq) at = i;
    for (unsigned i = 0; i < 4u; i++) if (g_mb[i].seq && g_mb[i].fp == fp && !strcmp(g_mb[i].name, name)){ at = i; break; }
    snprintf(g_mb[at].name, sizeof g_mb[at].name, "%s", name);
    g_mb[at].fp = fp; g_mb[at].l = g_cur_l; g_mb[at].seq = ++g_mseq;
    if (kept){ g_mk.on = 1; g_mk.p = p; g_mk.fp = fp; g_mk.l = g_cur_l; }
}
/* garage.c: anything to save beside the payload: the mod parts now against the kept payload's (a load itself is no
 * change: the profile as it is loads here) */
int modparts_changed(CPU *c, int p){
    if (!g_mk.on || g_mk.p != p || p < 0 || p > 1) return 0;
    static MList now;
    unconvert(CARS0_OF(p), p);                           /* (as the payload has them) */
    collect(c, CARS0_OF(p), p, &now, 0);
    reconvert();
    if (!lists_differ(&now, &g_mk.l)) return 0;
    fprintf(stderr, "[mods] the cars' mod parts changed: something to save\n");
    return 1;
}
static int newest_built(uint64_t fp, const char *name){
    int best = -1;
    for (int k = 0; k < 4; k++)
        if (g_mb[k].seq && g_mb[k].fp == fp && !strcasecmp(g_mb[k].name, name) && (best < 0 || g_mb[k].seq > g_mb[best].seq)) best = k;
    return best;
}
static void dir_of(const char *path, char *out, size_t n, const char *file){
    const char *sl = strrchr(path, '/');
    snprintf(out, n, "%.*s/%s", sl ? (int)(sl - path) : 1, sl ? path : ".", file);
}
/* the list l as a MODPARTS file at t, its head first (the profile name's, write counter count, the cars' fingerprint
 * fp of the payload it belongs to); 1 written whole */
static int write_list(const char *t, const MList *l, const char *name, uint32_t count, uint64_t fp){
    FILE *w = fopen(t, "wb");
    if (!w) return 0;
    uint32_t hd[4] = { MP_MAGIC, 1u, (uint32_t)l->n + 1u, count };
    uint32_t head[8] = { HEAD_MAGIC, name_hash(name), count, (uint32_t)fp, (uint32_t)(fp >> 32), 0, 0, K_HEAD };
    int ok = fwrite(hd, sizeof hd, 1, w) == 1 && fwrite(head, sizeof head, 1, w) == 1;
    for (int i = 0; i < l->n && ok; i++) ok = fwrite(l->e[i].w, sizeof l->e[i].w, 1, w) == 1;
    ok = fclose(w) == 0 && ok;
    if (!ok) remove(t);
    return ok;
}
static int read_one(const char *path, MList *l, uint32_t *count, uint32_t *nh, uint64_t *fp);
/* the write counter of the profile name: as its files had it (read the first time it is asked), then as written */
static uint32_t *count_of(const char *name){
    for (int i = 0; i < g_ncnt; i++) if (!strcasecmp(g_cnt[i].name, name)) return &g_cnt[i].count;
    int k = g_ncnt < 8 ? g_ncnt++ : 7;
    snprintf(g_cnt[k].name, sizeof g_cnt[k].name, "%s", name);
    g_cnt[k].count = 0;
    static MList tmp;
    for (int f = 0; f < 2; f++){
        char win[128], path[1100]; uint32_t c = 0, nh = 0; uint64_t fp = 0;
        snprintf(win, sizeof win, "NFS Underground 2\\%s\\%s", name, f ? MP_NEW : MP_FILE);
        if (fileio_resolve_save(win, path, sizeof path) && read_one(path, &tmp, &c, &nh, &fp) > 0 && c != 0xffffffffu && c > g_cnt[k].count)
            g_cnt[k].count = c;
    }
    return &g_cnt[k].count;
}
/* garage.c, a save (0x52A450): MODPARTS.NEW for the payload just made (its cars' fingerprint fp, the profile name),
 * before the game writes the profile; 1 written (or nothing to keep), 0 not: the payload is to be made again as it is */
int modparts_stage(int p, uint64_t fp, const char *name){
    g_staged.on = 0;
    int best = newest_built(fp, name);
    if (best < 0 || !name_ok(name)) return 1;            /* (no profile's: nothing beside it) */
    const MList *l = &g_mb[best].l;
    snprintf(g_staged.name, sizeof g_staged.name, "%s", name);
    g_staged.fp = fp; g_staged.empty = !l->n; g_staged.on = 1;
    if (!l->n) return 1;
    char win[128], t[1100] = "";
    snprintf(win, sizeof win, "NFS Underground 2\\%s\\%s", name, MP_NEW);
    fileio_resolve_save_w(win, t, sizeof t);              /* (0 for a file not there yet: its path all the same) */
    (void)p;
    g_staged.count = ++*count_of(name);
    if (!t[0] || !write_list(t, l, name, g_staged.count, fp)){
        g_staged.on = 0;
        fprintf(stderr, "[mods] %s: %s could not be written (no room?): the profile is saved with its parts as they are, "
                "those waiting too, this once\n", name, MP_NEW);
        return 0;
    }
    return 1;
}
/* f aside as the first of MODPARTS.OLD, .OLD2 ... .OLD9 not there yet; 1 kept */
static int keep_aside(const char *f, const char *path){
    for (int k = 1; k <= MP_OLDS; k++){
        char o[1100], nm[24];
        if (k == 1) snprintf(nm, sizeof nm, "%s", MP_OLD); else snprintf(nm, sizeof nm, "%s%d", MP_OLD, k);
        dir_of(path, o, sizeof o, nm);
        FILE *x = fopen(o, "rb");
        if (x){ fclose(x); continue; }
        return rename(f, o) == 0;
    }
    return 0;
}
/* MODPARTS.NEW at t over MODPARTS.DAT at f (a file this page cannot read there: kept aside first); 1 done */
static int finish(const char *name, const char *f, const char *t, const char *path){
    int fo = is_foreign(name);
    if (fo >= 0){                                        /* a file this page cannot read: kept aside, never lost */
        FILE *x = fopen(f, "rb");
        if (x){
            fclose(x);
            if (!keep_aside(f, path)){
                fprintf(stderr, "[mods] %s: its %s (not one this page reads) left as it is, %s not made %s (the next load takes "
                        "it by its counter)\n", name, MP_FILE, MP_NEW, MP_FILE);
                return 0;
            }
            fprintf(stderr, "[mods] %s: its %s (not one this page reads) kept aside as %s\n", name, MP_FILE, MP_OLD "*");
        }
        g_foreign[fo][0] = 0;
    }
    if (rename(t, f) != 0){ remove(f); if (rename(t, f) != 0){ fprintf(stderr, "[mods] %s: cannot rename %s\n", name, t); return 0; } }
    return 1;
}
/* fileio.c -> garage.c: the game has written a profile (host path; its cars' fingerprint, its name): its MODPARTS.NEW
 * staged for it becomes MODPARTS.DAT (none of its cars with a mod part: no file) */
void modparts_written(const char *path, uint64_t fp, const char *name){
    char f[1100], t[1100];
    dir_of(path, f, sizeof f, MP_FILE); dir_of(path, t, sizeof t, MP_NEW);
    int fo = is_foreign(name);
    if (!g_staged.on || g_staged.fp != fp || strcasecmp(g_staged.name, name)){
        /* (a payload of ours not staged: a save by another way than 0x52A450; written now, as a page of before did) */
        int best = newest_built(fp, name);
        if (best < 0 || !g_mb[best].l.n){ if (best >= 0 && fo < 0 && remove(f) == 0) fprintf(stderr, "[mods] %s: nothing kept beside it any more, its %s removed\n", name, MP_FILE); return; }
        if (!write_list(t, &g_mb[best].l, name, ++*count_of(name), fp)){ fprintf(stderr, "[mods] %s: WRITE FAILED, its %s as it was\n", name, MP_FILE); return; }
        g_staged.empty = 0;
    }
    g_staged.on = 0;
    if (g_staged.empty){
        remove(t);
        if (fo >= 0){ fprintf(stderr, "[mods] %s: its %s (not one this page reads) left as it is\n", name, MP_FILE); return; }
        if (remove(f) == 0) fprintf(stderr, "[mods] %s: nothing kept beside it any more, its %s removed\n", name, MP_FILE);
        return;
    }
    if (!finish(name, f, t, path)) return;
    int best = newest_built(fp, name), parts = 0, stances = 0, n = best >= 0 ? g_mb[best].l.n : 0;
    for (int i = 0; i < n; i++){ parts += E_KIND(&g_mb[best].l.e[i]) == K_PART; stances += E_KIND(&g_mb[best].l.e[i]) == K_STANCE; }
    char st[40] = "";
    if (stances) snprintf(st, sizeof st, ", %d %s", stances, stances == 1 ? "car's stance" : "cars' stances");
    fprintf(stderr, "[mods] %s: kept in %s: %d part%s (the profile has their stand-ins)%s%s\n", name, MP_FILE, parts,
            parts == 1 ? "" : "s", st, n > parts + stances ? ", and entries of a later page's" : "");
}
/* a MODPARTS file at path: 1 read (l, without its head; its write counter, its head's name hash and payload's cars'
 * fingerprint, 0 for none), 0 none, -1 one this page cannot read */
static int read_one(const char *path, MList *l, uint32_t *count, uint32_t *nh, uint64_t *fp){
    l->n = 0; *count = 0; *nh = 0; *fp = 0;
    FILE *in = fopen(path, "rb");
    if (!in) return 0;
    uint32_t hd[4] = { 0 }, hc = 0;
    int ok = fread(hd, sizeof hd, 1, in) == 1 && hd[0] == MP_MAGIC && hd[1] == 1u && hd[2] <= MP_MAX + 1u, head = 0;
    for (uint32_t i = 0; ok && i < hd[2]; i++){
        MPart *e = &l->e[l->n];
        memset(e, 0, sizeof *e);
        ok = fread(e->w, sizeof e->w, 1, in) == 1 && (E_KIND(e) || E_SLOT(e) < N_SLOTS);
        if (ok && E_KIND(e) == K_HEAD && e->w[0] == HEAD_MAGIC && !head){ head = 1; *nh = e->w[1]; hc = e->w[2]; *fp = (uint64_t)e->w[3] | (uint64_t)e->w[4] << 32; continue; }
        if (ok && l->n == MP_MAX) ok = 0;
        if (ok) l->n++;
    }
    ok = ok && fgetc(in) == EOF;
    fclose(in);
    if (!ok){ l->n = 0; return -1; }
    /* the head word: this page's counter; 0 with a head of ours inside: a page of before wrote it last (the newest) */
    *count = hd[3] ? hd[3] : head ? 0xffffffffu : 0;
    (void)hc;
    return 1;
}
/* a profile's MODPARTS file, for its payload as loaded (its cars' fingerprint fp): MODPARTS.NEW where it belongs to
 * this very payload (the profile written after it, the rename cut) and is not older than MODPARTS.DAT (by their write
 * counters: two saves of the same cars), renamed MODPARTS.DAT now; else MODPARTS.DAT, and a MODPARTS.NEW of a payload
 * not written (a save cut before the profile) removed.  1 read (l), 0 none, -1 MODPARTS.DAT is one this page cannot
 * read (and no MODPARTS.NEW of this payload) */
static int read_mp(const char *name, uint64_t fp, MList *l){
    char win[128], df[1100] = "", nf[1100] = "";
    static MList nw;
    uint32_t cd = 0, cn = 0, hd_ = 0, hn = 0; uint64_t fd = 0, fnw = 0;
    snprintf(win, sizeof win, "NFS Underground 2\\%s\\%s", name, MP_FILE);
    int d = fileio_resolve_save(win, df, sizeof df) ? read_one(df, l, &cd, &hd_, &fd) : 0;
    if (!d) fileio_resolve_save_w(win, df, sizeof df);
    snprintf(win, sizeof win, "NFS Underground 2\\%s\\%s", name, MP_NEW);
    int w = fileio_resolve_save(win, nf, sizeof nf) ? read_one(nf, &nw, &cn, &hn, &fnw) : 0;
    if (d < 0 && is_foreign(name) < 0){ int k = g_nforeign < 8 ? g_nforeign++ : 7; snprintf(g_foreign[k], sizeof g_foreign[k], "%s", name); }
    int mine = w > 0 && hn == name_hash(name) && fnw == fp && fp;
    uint32_t *cnt = count_of(name);
    if (d > 0 && cd != 0xffffffffu && cd > *cnt) *cnt = cd;
    if (w > 0 && cn != 0xffffffffu && cn > *cnt) *cnt = cn;
    /* DAT wins only as a later save of the same cars: a page of before's DAT never does, since such a page writes through
     * MODPARTS.NEW and renames it, so a NEW of ours beside its DAT came after it */
    int dat_too = d > 0 && fd == fp && cd != 0xffffffffu && cd >= cn;
    if (mine && !dat_too){
        *l = nw;
        fprintf(stderr, "[mods] %s: its %s (write %u, of the profile as it is: its rename cut) taken, renamed %s now\n", name, MP_NEW, cn, MP_FILE);
        if (df[0]) finish(name, df, nf, df);
        return 1;
    }
    if (w && !mine && hn == name_hash(name) && remove(nf) == 0)
        fprintf(stderr, "[mods] %s: its %s (of a save whose profile was not written) removed\n", name, MP_NEW);
    if (d > 0) return 1;
    if (d < 0){ l->n = 0; fprintf(stderr, "[mods] %s: its %s is not one this page reads: not used, kept as it is\n", name, MP_FILE); return -1; }
    return 0;
}
/* garage.c: player p's cars (db) loaded from a payload (in; its cars' fingerprint, its profile's name, "" if not
 * known; kept: the one the game keeps, going back to it): the mod parts back on their cars, those this game lacks
 * (and any of another kind) waiting */
void modparts_loaded(CPU *c, uint32_t db, int p, uint32_t in, uint64_t fp, const char *name, int kept){
    (void)c; (void)in;
    if (p < 0 || p > 1) return;
    g_add[p].n = 0;                                      /* (the cars are the payload's now: garage.c's addon_cars next) */
    static MList l;
    if (kept && g_mk.on && g_mk.p == p && g_mk.fp == fp){ l = g_mk.l; g_wait[p].n = 0; }   /* (the same profile's) */
    else if (!name_ok(name)) return;                     /* (no profile's: nothing of it changed) */
    else { if (read_mp(name, fp, &l) <= 0) l.n = 0; g_wait[p].n = 0; }
    if (name_ok(name)) snprintf(g_wait_name[p], sizeof g_wait_name[p], "%s", name);
    stance_loaded(p, name);                              /* (the cars' stances: the file's, none without) */
    int back = 0, wait = 0, gone = 0, stances = 0;
    static uint32_t at[MP_MAX];
    for (int i = 0; i < l.n; i++){                       /* each against its car as loaded (a car's several, first) */
        const MPart *e = &l.e[i];
        at[i] = 0;
        if (E_KIND(e) == K_STANCE){                      /* a stance: its car as it was saved (1), changed elsewhere (2) */
            uint32_t car = car_of(db, E_HANDLE(e), E_TYPE(e));
            at[i] = !car ? 0 : car_fp(car) == E_FP(e) ? 1u : 2u;
            continue;
        }
        if (E_KIND(e)) continue;
        uint32_t car = car_of(db, E_HANDLE(e), E_TYPE(e));
        at[i] = car ? KEY_AT(car, E_SLOT(e)) : 0;
        if (!car || MR32(at[i]) != E_STAND(e) || car_fp(car) != E_FP(e)){ at[i] = 0; gone++; }   /* changed elsewhere: as it is */
    }
    for (int i = 0; i < l.n; i++){
        const MPart *e = &l.e[i];
        if (E_KIND(e) == K_HEAD) continue;
        /* a stance of a car changed elsewhere (another car of that handle and type, or the same one with other parts:
         * the PC game, a page of before) is not drawn, but waits as it is (a stance set here later takes its place) */
        if (E_KIND(e) == K_STANCE && at[i] == 1u && stance_load(p, e->w)){ stances++; continue; }
        if (E_KIND(e) == K_STANCE && at[i] == 2u) gone++;
        if (E_KIND(e)){ if (g_wait[p].n < MP_MAX) g_wait[p].e[g_wait[p].n++] = *e; continue; }   /* another kind: kept */
        if (!at[i]) continue;
        /* (a car of a car type this game has not stays as loaded: its part waits, and the payload has it as it was) */
        if (have_key(E_KEY(e)) && modparts_cartype(E_TYPE(e)) > 0){ MR32(at[i]) = E_KEY(e); back++; }
        else if (g_wait[p].n < MP_MAX){ g_wait[p].e[g_wait[p].n++] = *e; wait++; }
    }
    if (!l.n) return;
    fprintf(stderr, "[mods] %s%s: %d part%s back on its cars", name[0] ? name : "(the cars)", kept ? " (the kept payload)" : "",
            back, back == 1 ? "" : "s");
    if (wait) fprintf(stderr, ", %d this game has not (MODPACKS, VINYLPACKS): the stand-ins meanwhile, kept for later", wait);
    if (gone) fprintf(stderr, ", %d whose car changed elsewhere: as it is", gone);
    if (stances) fprintf(stderr, ", %d car%s with a stance of its own", stances, stances == 1 ? "" : "s");
    fprintf(stderr, "\n");
}
/* garage.c: the profile name deleted: nothing of it waits any more (a new one of that name starts as new) */
void modparts_deleted(const char *name){
    stance_deleted(name);
    for (int p = 0; p < 2; p++){
        if (strcasecmp(g_wait_name[p], name)) continue;
        g_wait[p].n = 0; g_wait_name[p][0] = 0;
    }
    for (int k = 0; k < 4; k++) if (!strcasecmp(g_mb[k].name, name)) g_mb[k].seq = 0, g_mb[k].name[0] = 0;
    for (int k = 0; k < g_ncnt; k++) if (!strcasecmp(g_cnt[k].name, name)) g_cnt[k].count = 0;
    int f = is_foreign(name);
    if (f >= 0) g_foreign[f][0] = 0;
    if (g_staged.on && !strcasecmp(g_staged.name, name)) g_staged.on = 0;
}

/* --- other players' cars: the mods' parts, or the game's --------------------
 * others_mod_parts = original (NFSU2_OTHERS_MOD_PARTS): other players' cars
 * without the mods' parts, each one's stand-in in its place (its look-alike,
 * a game's rim of the same size, else the slot's stock part), the player's
 * own car as it is: free roam's cars of others (roam_game.c build_ride) and a
 * race's (the game's LAN car message, 0x5F04D0: thiscall (ride, message, ?),
 * ret 0xC, the ride Init'ed (+0 its car type) and its parts set). */
#define FN_GETCARPART  0x0061b980u  /* thiscall CarPartDB (type, slot, key, prev, -1) */
#define FN_PARTS_ON    0x0061bcd0u  /* thiscall ride: UpdatePartsEnabled */
int modpacks_others_original(void){
    static int v = -1;
    if (v < 0){
        const char *e = getenv("NFSU2_OTHERS_MOD_PARTS");
        v = e && !strcasecmp(e, "original");
        if (v) fprintf(stderr, "[mods] other players' cars: the game's parts in place of the mods' (others_mod_parts = original)\n");
    }
    return v;
}
/* roam_game.c: a friend's car's part key -> the key to show it with (0: the slot's stock part); the key itself where
 * the mods' parts are shown, or for a part of the game's */
uint32_t modpacks_for_others(uint32_t key){
    if (!modpacks_others_original() || !mod_key(key)) return key;
    uint32_t la = lookalike(key);
    static int said;
    if (said++ < 8) fprintf(stderr, "[mods] another player's car: its mod part %08x shown as %s %08x\n", key, la ? "its look-alike" : "the stock part", la);
    return la;
}
/* the stock part of a ride's slot (a ride of ours Init'ed as it was: its car type, +5, +6) */
static uint32_t stock_part(CPU *c, uint32_t ride, uint32_t slot){
    if (!g_ride && !(g_ride = gheap_alloc(1, 0x1000u, 1))) return 0;
    for (uint32_t o = 0; o < 0x1000u; o += 4u) MR32(g_ride + o) = 0;
    uint32_t a[4] = { MR32(ride), 0, MR8(ride + 5u), MR8(ride + 6u) }, z = 0;
    gcall(c, FN_RIDE_INIT, g_ride, 4, a);
    gcall(c, FN_RIDE_STOCK, g_ride, 1, &z);
    return MR32(g_ride + RIDE_PARTS + 4u * slot);
}
void sub_005f04d0_orig(CPU *c);
void sub_005f04d0(CPU *c){
    uint32_t ride = MR32(c->esp + 4u);
    sub_005f04d0_orig(c);
    if (!modpacks_others_original() || !g_grown || !guest_ok_bytes(ride, 0x8f0u)) return;
    int n = 0, la_n = 0;
    for (uint32_t s = 0; s < N_SLOTS; s++){
        uint32_t part = MR32(ride + RIDE_PARTS + 4u * s);
        if (!part || !guest_ok_bytes(part, 4u) || !mod_key(MR32(part))) continue;
        uint32_t la = lookalike(MR32(part)), np = 0;
        if (la){ uint32_t a[5] = { MR32(ride), s, la, 0, 0xffffffffu }; np = gcall(c, FN_GETCARPART, G_CARPARTDB, 5, a); }
        if (np) la_n++; else np = stock_part(c, ride, s);
        MR32(ride + RIDE_PARTS + 4u * s) = np; n++;
    }
    if (!n) return;
    gcall(c, FN_PARTS_ON, ride, 0, 0);
    static int said;
    if (said++ < 8) fprintf(stderr, "[mods] a race's car (ride 0x%08x): %d mod part%s shown as the game's (%d look-alike%s, the rest stock)\n",
                            ride, n, n == 1 ? "" : "s", la_n, la_n == 1 ? "" : "s");
}
