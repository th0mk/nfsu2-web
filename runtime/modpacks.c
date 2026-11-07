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
static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n - 1u); }
static uint32_t rd32(const uint8_t *b){ return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static const char *pstr(uint32_t o){ return o < g.nstr ? (const char *)g.str + o : 0; }
static int switched_on(const char *env){
    const char *e = getenv(env);   /* the page writes 1 or 0; the Mac app passes the line's own on/off */
    return e && *e && *e != '0' && strcasecmp(e, "off") && strcasecmp(e, "no") && strcasecmp(e, "false");
}

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
         * settings.ini says; back: g.on[i] = switched_on(g.env[i]) */
        g.on[i] = 0; (void)switched_on;
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
    if (!ok(chunk, 16u) || MR32(chunk) != CHUNK_PARTS || !ok(chunk, MR32(chunk + 4u) + 8u) || !load()) return 0;
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
 * "NFS Underground 2/<NAME>/MODPARTS.DAT", written when the profile is
 * (fileio.c -> garage.c).  When a profile's cars load (0x516850, garage.c),
 * each goes back on its car where that car is still as it was saved: found
 * by its handle, of the same car type, the stand-in in the slot and its parts
 * as saved (a fingerprint of its 170 keys, the stand-ins among them).  A car
 * changed elsewhere (the original game, a page of before) keeps what it has.
 * A part this game lacks (no MODPACKS) waits and is written again with that
 * profile's next save while its car keeps the stand-in.  The original game, a
 * page of before and a game without MODPACKS see the stand-ins: the stock
 * parts.  The game's "anything to save?" (0x52A4C0) counts a change of them.
 *
 * The cars with parts a player can change: the table's 20 Customize cars
 * (+0x544, 0x434 bytes each) and the career's 5 (0x7F8), garage.c's slots
 * 6-10 (garage_records; the table's 48 stock and 12 other records are 0x1C
 * bytes, its 6 presets the game's own).  Their parts at +0x18: +8 the car
 * type, +0xC + 4 x slot each of the 170 slots' part key (0: none), as 0x503880
 * makes a ride of them and 0x503950 writes a ride back.
 *
 * MODPARTS.DAT: 'N2MP', version 1, count, 0, then count entries of 8 words:
 * {handle, car type, slot, the mod part's key, the stand-in's key, the car's
 * fingerprint (2 words), kind}; kind 0 a slot's part.  An entry of another
 * kind (a later page's) is kept as it is and written again; a file this page
 * cannot read (another version, cut short) is never written over or removed:
 * a save with mod parts of its own first keeps it aside as MODPARTS.OLD.  It
 * is written as MODPARTS.NEW, then renamed: never cut short. */
int garage_records(uint32_t db, uint32_t *out, int max);
int fileio_resolve_save(const char *win, char *out, size_t outsz);
void dispatch_indirect(CPU *c, uint32_t addr);

#define MP_MAGIC      0x504d324eu   /* "N2MP" */
#define MP_FILE       "MODPARTS.DAT"
#define MP_NEW        "MODPARTS.NEW"
#define MP_OLD        "MODPARTS.OLD"
#define MP_MAX        1024
#define R_HANDLE      0x08u
#define R_PARTS       0x18u         /* +8 car type, +0xC + 4 x slot: the keys */
#define N_SLOTS       0xaau
#define FN_TYPE_INFO  0x00610130u   /* cdecl (car type) -> its info (+0x840: what a ride's Init takes) */
#define FN_RIDE_INIT  0x00610270u   /* thiscall ride (that, 1, 0, 0) */
#define FN_RIDE_STOCK 0x00637040u   /* thiscall ride (0): every slot its stock part */
#define RIDE_PARTS    0x590u        /* a ride's parts: +4 x slot */
#define CARS0_OF(p)   (0x0083ad90u + (uint32_t)(p) * 0xab4cu)   /* a player's cars (garage.c CARS0, CARS_SZ) */

typedef struct { uint32_t w[8]; uint8_t wait; } MPart;  /* w: as the file has it; wait: one of g_wait's */
#define E_HANDLE(e)   ((e)->w[0])
#define E_TYPE(e)     ((e)->w[1])
#define E_SLOT(e)     ((e)->w[2])
#define E_KEY(e)      ((e)->w[3])
#define E_STAND(e)    ((e)->w[4])
#define E_FP(e)       ((uint64_t)(e)->w[5] | (uint64_t)(e)->w[6] << 32)
#define E_KIND(e)     ((e)->w[7])
typedef struct { MPart e[MP_MAX]; int n; } MList;
static MList g_wait[2];                                  /* each player's parts not here (no MODPACKS), and other kinds */
static char g_wait_name[2][33];                          /* ... of that profile */
static MList g_cur_l;                                    /* the payload being made */
static struct { uint32_t at, key; } g_hid[MP_MAX * 2]; static int g_nhid;
static struct { char name[33]; uint64_t fp; MList l; unsigned seq; } g_mb[4];   /* the payloads made, newest kept */
static unsigned g_mseq;
static struct { int on, p; uint64_t fp; MList l; } g_mk;                        /* the one the game keeps */
static char g_foreign[8][33]; static int g_nforeign;    /* profiles whose MODPARTS.DAT this page cannot read */
static uint32_t g_ride;                                  /* a ride of ours, for the stock parts */
static struct { uint32_t type, key[N_SLOTS]; } g_stock[16]; static int g_nstock;

static uint32_t mcall(CPU *c, uint32_t fn, uint32_t ecx, int n, const uint32_t *arg){
    CPU save = *c;
    c->esp = (c->esp - 256u) & ~15u;
    for (int i = n - 1; i >= 0; i--) PUSH32(arg[i]);
    c->ecx = ecx;
    PUSH32(fn);                                          /* the callee's ret pops it */
    dispatch_indirect(c, fn);
    uint32_t r = c->eax;
    *c = save;
    return r;
}
static int mod_key(uint32_t key){
    if (!key || !g_grown) return 0;
    for (int i = 0; i < g.nparts; i++) if (g.p[i].key == key) return 1;
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
    uint32_t info = mcall(c, FN_TYPE_INFO, 0, 1, &type);
    int i = g_nstock < 16 ? g_nstock++ : 15;
    g_stock[i].type = type;
    memset(g_stock[i].key, 0, sizeof g_stock[i].key);
    if (!info || !mem_is_committed(info + 0x840u)){ fprintf(stderr, "[mods] car type %08x: no info for its stock parts\n", type); return 0; }
    for (uint32_t o = 0; o < 0x1000u; o += 4u) MR32(g_ride + o) = 0;
    uint32_t a[4] = { MR32(info + 0x840u), 1u, 0, 0 }, z = 0;
    mcall(c, FN_RIDE_INIT, g_ride, 4, a);
    mcall(c, FN_RIDE_STOCK, g_ride, 1, &z);
    for (uint32_t s = 0; s < N_SLOTS; s++){
        uint32_t part = MR32(g_ride + RIDE_PARTS + 4u * s);
        g_stock[i].key[s] = part && mem_is_committed(part + 3u) ? MR32(part) : 0;
    }
    return g_stock[i].key[slot];
}
static uint64_t car_fp(uint32_t r){
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t o = 8u; o < 0xcu + 4u * N_SLOTS; o++){ h ^= MR8(r + R_PARTS + o); h *= 0x100000001b3ull; }
    return h;
}
static uint32_t car_of(uint32_t db, uint32_t handle, uint32_t type){
    uint32_t r[64]; int n = garage_records(db, r, 64);
    for (int i = 0; i < n; i++) if (MR32(r[i] + R_HANDLE) == handle && MR32(r[i] + R_PARTS + 8u) == type) return r[i];
    return 0;
}
static int lists_differ(const MList *a, const MList *b){
    if (a->n != b->n) return 1;
    for (int i = 0; i < a->n; i++)
        if (E_HANDLE(&a->e[i]) != E_HANDLE(&b->e[i]) || E_SLOT(&a->e[i]) != E_SLOT(&b->e[i]) || E_KEY(&a->e[i]) != E_KEY(&b->e[i]) ||
            E_KIND(&a->e[i]) != E_KIND(&b->e[i])) return 1;
    return 0;
}
static int is_foreign(const char *name){
    for (int i = 0; i < g_nforeign; i++) if (!strcasecmp(g_foreign[i], name)) return i;
    return -1;
}
/* the cars' mod parts now (hide: the stand-ins put in for the while; every mod part goes out of the cars even when
 * the list is full), the waiting ones with them */
static void collect(CPU *c, uint32_t db, int p, MList *l, int hide){
    uint32_t r[64]; int n = garage_records(db, r, 64), full = 0;
    l->n = 0;
    if (hide) g_nhid = 0;
    for (int i = 0; i < n; i++){
        uint32_t handle = MR32(r[i] + R_HANDLE), type = MR32(r[i] + R_PARTS + 8u);
        for (uint32_t s = 0; s < N_SLOTS; s++){
            uint32_t at = r[i] + R_PARTS + 0xcu + 4u * s, key = MR32(at);
            if (!mod_key(key)) continue;
            uint32_t stand = lookalike(key);
            if (!stand) stand = stock_key(c, type, s);
            if (l->n < MP_MAX){
                MPart *e = &l->e[l->n++];
                memset(e, 0, sizeof *e);
                e->w[0] = handle; e->w[1] = type; e->w[2] = s; e->w[3] = key; e->w[4] = stand;
            } else full++;
            if (hide && g_nhid < MP_MAX * 2){ g_hid[g_nhid].at = at; g_hid[g_nhid].key = key; g_nhid++; MR32(at) = stand; }
        }
    }
    if (full) fprintf(stderr, "[mods] %d mod part%s more than MODPARTS.DAT holds: the stock ones in the profile, not kept\n", full, full == 1 ? "" : "s");
    for (int i = 0; i < g_wait[p].n && l->n < MP_MAX; i++){
        const MPart *w = &g_wait[p].e[i];
        if (E_KIND(w)){ l->e[l->n] = *w; l->e[l->n++].wait = 1; continue; }        /* another kind: as it was */
        uint32_t car = car_of(db, E_HANDLE(w), E_TYPE(w));
        if (car && MR32(car + R_PARTS + 0xcu + 4u * E_SLOT(w)) == E_STAND(w)){ l->e[l->n] = *w; l->e[l->n++].wait = 1; }
    }
    for (int i = 0; i < l->n; i++){
        if (E_KIND(&l->e[i])) continue;
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
/* garage.c: a payload is being made of player p's cars (db) */
void modparts_hide(CPU *c, uint32_t db, int p){
    if (p < 0 || p > 1) return;
    collect(c, db, p, &g_cur_l, 1);
}
/* ... the cars as they were */
void modparts_show(void){
    for (int i = 0; i < g_nhid; i++) MR32(g_hid[i].at) = g_hid[i].key;
    g_nhid = 0;
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
/* garage.c: anything to save beside the payload: the mod parts now against the kept payload's */
int modparts_changed(CPU *c, int p){
    if (!g_mk.on || g_mk.p != p || p < 0 || p > 1) return 0;
    static MList now;
    collect(c, CARS0_OF(p), p, &now, 0);
    if (!lists_differ(&now, &g_mk.l)) return 0;
    fprintf(stderr, "[mods] the cars' mod parts changed: something to save\n");
    return 1;
}
/* fileio.c -> garage.c: the game has written a profile (host path; its cars' fingerprint, its name): its
 * MODPARTS.DAT beside it, from the payload of ours it was made of (none of its cars with a mod part: no file) */
void modparts_written(const char *path, uint64_t fp, const char *name){
    int best = -1;
    for (int k = 0; k < 4; k++)
        if (g_mb[k].seq && g_mb[k].fp == fp && !strcasecmp(g_mb[k].name, name) && (best < 0 || g_mb[k].seq > g_mb[best].seq)) best = k;
    if (best < 0) return;                                /* (garage.c says so) */
    const MList *l = &g_mb[best].l;
    char f[1100], t[1100], o[1100]; const char *sl = strrchr(path, '/');
    int dl = sl ? (int)(sl - path) : 1; const char *dir = sl ? path : ".";
    snprintf(f, sizeof f, "%.*s/%s", dl, dir, MP_FILE);
    snprintf(t, sizeof t, "%.*s/%s", dl, dir, MP_NEW);
    snprintf(o, sizeof o, "%.*s/%s", dl, dir, MP_OLD);
    int fo = is_foreign(name);
    if (!l->n){
        if (fo >= 0){ fprintf(stderr, "[mods] %s: its %s (not one this page reads) left as it is\n", name, MP_FILE); return; }
        if (remove(f) == 0) fprintf(stderr, "[mods] %s: no mod parts on its cars any more, its %s removed\n", name, MP_FILE);
        return;
    }
    FILE *w = fopen(t, "wb");
    if (!w){ fprintf(stderr, "[mods] %s: cannot write %s\n", name, t); return; }
    uint32_t hd[4] = { MP_MAGIC, 1u, (uint32_t)l->n, 0 };
    int ok = fwrite(hd, sizeof hd, 1, w) == 1;
    for (int i = 0; i < l->n && ok; i++) ok = fwrite(l->e[i].w, sizeof l->e[i].w, 1, w) == 1;
    ok = fclose(w) == 0 && ok;
    if (!ok){ remove(t); fprintf(stderr, "[mods] %s: WRITE FAILED, its %s as it was\n", name, MP_FILE); return; }
    if (fo >= 0){                                        /* a file this page cannot read: kept aside, never lost */
        remove(o);
        int kept = rename(f, o) == 0;
        fprintf(stderr, "[mods] %s: its %s (not one this page reads) %s\n", name, MP_FILE, kept ? "kept as " MP_OLD : "COULD NOT BE KEPT ASIDE");
        if (!kept){ remove(t); return; }
        g_foreign[fo][0] = 0;
    }
    if (rename(t, f) != 0){ remove(f); if (rename(t, f) != 0){ fprintf(stderr, "[mods] %s: cannot rename %s\n", name, t); return; } }
    fprintf(stderr, "[mods] %s: %d mod part%s of its cars kept in %s (the profile has their stock parts)\n",
            name, l->n, l->n == 1 ? "" : "s", MP_FILE);
}
/* a profile's MODPARTS.DAT: 1 read (l: its entries), 0 none, -1 one this page cannot read */
static int read_mp(const char *name, MList *l){
    char win[128], path[1100];
    l->n = 0;
    snprintf(win, sizeof win, "NFS Underground 2\\%s\\%s", name, MP_FILE);
    if (!fileio_resolve_save(win, path, sizeof path)) return 0;
    FILE *in = fopen(path, "rb");
    if (!in) return 0;
    uint32_t hd[4] = { 0 };
    int ok = fread(hd, sizeof hd, 1, in) == 1 && hd[0] == MP_MAGIC && hd[1] == 1u && hd[2] <= MP_MAX;
    for (uint32_t i = 0; ok && i < hd[2]; i++){
        MPart *e = &l->e[l->n++];
        memset(e, 0, sizeof *e);
        ok = fread(e->w, sizeof e->w, 1, in) == 1 && (E_KIND(e) || E_SLOT(e) < N_SLOTS);
    }
    ok = ok && fgetc(in) == EOF;
    fclose(in);
    if (!ok){
        l->n = 0;
        if (is_foreign(name) < 0){ int k = g_nforeign < 8 ? g_nforeign++ : 7; snprintf(g_foreign[k], sizeof g_foreign[k], "%s", name); }
        fprintf(stderr, "[mods] %s: its %s is not one this page reads: not used, kept as it is\n", name, MP_FILE);
        return -1;
    }
    return 1;
}
static int name_ok(const char *s){
    if (!s[0]) return 0;
    for (; *s; s++) if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '-')) return 0;
    return 1;
}
/* garage.c: player p's cars (db) loaded from a payload (in; its cars' fingerprint, its profile's name, "" if not
 * known; kept: the one the game keeps, going back to it): the mod parts back on their cars, those this game lacks
 * (and any of another kind) waiting */
void modparts_loaded(CPU *c, uint32_t db, int p, uint32_t in, uint64_t fp, const char *name, int kept){
    (void)c; (void)in;
    if (p < 0 || p > 1) return;
    static MList l;
    if (kept && g_mk.on && g_mk.p == p && g_mk.fp == fp){ l = g_mk.l; g_wait[p].n = 0; }   /* (the same profile's) */
    else if (!name_ok(name)) return;                     /* (no profile's: nothing of it changed) */
    else { if (read_mp(name, &l) <= 0) l.n = 0; g_wait[p].n = 0; }
    if (name_ok(name)) snprintf(g_wait_name[p], sizeof g_wait_name[p], "%s", name);
    int back = 0, wait = 0, gone = 0;
    static uint32_t at[MP_MAX];
    for (int i = 0; i < l.n; i++){                       /* each against its car as loaded (a car's several, first) */
        const MPart *e = &l.e[i];
        at[i] = 0;
        if (E_KIND(e)) continue;
        uint32_t car = car_of(db, E_HANDLE(e), E_TYPE(e));
        at[i] = car ? car + R_PARTS + 0xcu + 4u * E_SLOT(e) : 0;
        if (!car || MR32(at[i]) != E_STAND(e) || car_fp(car) != E_FP(e)){ at[i] = 0; gone++; }   /* changed elsewhere: as it is */
    }
    for (int i = 0; i < l.n; i++){
        const MPart *e = &l.e[i];
        if (E_KIND(e)){ if (g_wait[p].n < MP_MAX) g_wait[p].e[g_wait[p].n++] = *e; continue; }   /* another kind: kept */
        if (!at[i]) continue;
        if (mod_key(E_KEY(e))){ MR32(at[i]) = E_KEY(e); back++; }
        else if (g_wait[p].n < MP_MAX){ g_wait[p].e[g_wait[p].n++] = *e; wait++; }
    }
    if (!l.n) return;
    fprintf(stderr, "[mods] %s%s: %d mod part%s back on its cars", name[0] ? name : "(the cars)", kept ? " (the kept payload)" : "",
            back, back == 1 ? "" : "s");
    if (wait) fprintf(stderr, ", %d this game has not (MODPACKS): the stock ones meanwhile, kept for later", wait);
    if (gone) fprintf(stderr, ", %d whose car changed elsewhere: as it is", gone);
    fprintf(stderr, "\n");
}

/* --- other players' cars: the mods' parts, or the game's --------------------
 * others_mod_parts = original (NFSU2_OTHERS_MOD_PARTS): other players' cars
 * without the mods' parts, each one's stand-in in its place (its look-alike,
 * a game's rim of the same size, else the slot's stock part), the player's
 * own car as it is: free roam's cars of others (roam_game.c build_ride) and a
 * race's (the game's LAN car message, 0x5F04D0: thiscall (ride, message, ?),
 * ret 0xC, the ride Init'ed (+0 its car type) and its parts set). */
#define FN_GETCARPART  0x0061b980u  /* thiscall CarPartDB (type, slot, key, prev, -1) */
#define G_CARPARTDB    0x008a2b68u
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
    mcall(c, FN_RIDE_INIT, g_ride, 4, a);
    mcall(c, FN_RIDE_STOCK, g_ride, 1, &z);
    return MR32(g_ride + RIDE_PARTS + 4u * slot);
}
void sub_005f04d0_orig(CPU *c);
void sub_005f04d0(CPU *c){
    uint32_t ride = MR32(c->esp + 4u);
    sub_005f04d0_orig(c);
    if (!modpacks_others_original() || !g_grown || !ok(ride, 0x8f0u)) return;
    int n = 0, la_n = 0;
    for (uint32_t s = 0; s < N_SLOTS; s++){
        uint32_t part = MR32(ride + RIDE_PARTS + 4u * s);
        if (!part || !ok(part, 4u) || !mod_key(MR32(part))) continue;
        uint32_t la = lookalike(MR32(part)), np = 0;
        if (la){ uint32_t a[5] = { MR32(ride), s, la, 0, 0xffffffffu }; np = mcall(c, FN_GETCARPART, G_CARPARTDB, 5, a); }
        if (np) la_n++; else np = stock_part(c, ride, s);
        MR32(ride + RIDE_PARTS + 4u * s) = np; n++;
    }
    if (!n) return;
    mcall(c, FN_PARTS_ON, ride, 0, 0);
    static int said;
    if (said++ < 8) fprintf(stderr, "[mods] a race's car (ride 0x%08x): %d mod part%s shown as the game's (%d look-alike%s, the rest stock)\n",
                            ride, n, n == 1 ? "" : "s", la_n, la_n == 1 ? "" : "s");
}
