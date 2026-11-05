/* vinylpacks.c -- the vinyls of the Japanese release (Need for Speed
 * Underground 2 Sha_Do, PlayStation 2, 2005): Art Factory 28 to 34, Sha_Do's
 * designs for Art Factory Graphics, which the other releases never had.  They
 * join the Art Factory category after Art Factory 27, as the Japanese game has
 * them, on every car.  Their textures come from the vinyl packs
 * (tools/vinylpacks.py; runtime/fileio.c reads each car's VINYLS.BIN with its
 * pack's textures added); without the packs nothing is added.
 *
 * The car part database is GLOBALB's chunk 0x80034602, which the game's loader
 * (0x636BE0, a bChunkLoader's, reached only through its pointer) makes the
 * database of: its parts point into the chunk's tables, and it lists the parts
 * of each kind, category and level as a first part and a count, so a kind's
 * parts must stay together.  Here the loader is given a copy of the chunk with
 * the seven parts in it, right after ART_F_27, which they copy: the GLOBALB
 * file and the game's memory of it stay as they are (mprace.c's sums of the
 * car records too).  The chunk, as the loader walks it (by each sub-chunk's
 * size, in this order):
 *   0x34603  header: +0x20 the attributes' count, +0x38 the parts' count (the
 *            loader writes the tables' places over the rest)
 *   0x34606  strings, each at a 4-byte boundary, named by index (x4)
 *   0x3460C  attribute lists: a count, then that many attribute indices (s16)
 *   0x34605  attributes: { hash, value } (8 bytes)
 *   0x3460A  model names (0x24 each)
 *   0x3460B  owners
 *   0x34604  parts, 14 bytes each: +0 key (the hash of its texture's name),
 *            +4 kind (0x32 a vinyl), +5 category (low 5 bits; 0x1A Art
 *            Factory) and unlock level (top 3), +7 owner, +8 its name's
 *            string, +0xA its attribute list, +0xC its models (0xFFFF none)
 * An Art Factory vinyl's attributes: 0x10C98090 its key, 0xFD35FE70 its
 * texture's name ("ART_F_27": a car's texture is "<CAR>_ART_F_27", its mask
 * "<CAR>_ART_F_27_MASK"), and four the vinyls share.  The new parts copy
 * ART_F_27's (unlock level, price, owner) with their own key, names and those
 * two attributes. */
#include "cpu.h"
#include <stdio.h>
#include <string.h>

void dispatch_patch(uint32_t a, void (*f)(CPU *));
void sub_00636be0(CPU *c);                     /* the lifted loader */
uint32_t gheap_alloc(int heap, uint32_t n, int zero);
int fileio_vinyl_packs(void);

#define FN_LOADER     0x00636be0u
#define CHUNK_PARTS   0x80034602u
#define ATTR_KEY      0x10c98090u
#define ATTR_TEXTURE  0xfd35fe70u
#define KIND_VINYL    0x32u
#define NEW_N         7
#define FIRST_NEW     28
#define PART_SZ       14u

static const uint32_t SUB[7] = { 0x34603u, 0x34606u, 0x3460cu, 0x34605u, 0x3460au, 0x3460bu, 0x34604u };
enum { S_HDR, S_STR, S_LIST, S_ATTR, S_MODEL, S_OWNER, S_PART };

static uint32_t bhash(const char *s){
    uint32_t h = 0xffffffffu;
    for (; *s; s++) h = h * 33u + (uint8_t)*s;
    return h;
}
static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n - 1u); }

/* the chunk with the seven parts in it, in guest memory for good; 0: as it is (logged why) */
static uint32_t grown(uint32_t chunk){
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
    if (!laid){ fprintf(stderr, "[vinyls] the car part database is laid out unlike the game's: no Sha_Do vinyls\n"); return 0; }
    uint32_t hdr = sub[S_HDR][0] + 8u, lst = sub[S_LIST][0] + 8u, atr = sub[S_ATTR][0] + 8u;
    uint32_t prt = sub[S_PART][0] + 8u, nattr = MR32(hdr + 0x20u), nparts = MR32(hdr + 0x38u);
    if (sub[S_HDR][1] < 0x3cu || nattr * 8u > sub[S_ATTR][1] || nparts * PART_SZ > sub[S_PART][1] || sub[S_STR][1] & 3u){
        fprintf(stderr, "[vinyls] the car part database's counts are not its tables': no Sha_Do vinyls\n"); return 0;
    }
    uint32_t key27 = bhash("ART_F_27"), p27 = 0;
    uint32_t keys[NEW_N];
    char tex[NEW_N][16], name[NEW_N][20];
    for (int k = 0; k < NEW_N; k++){
        snprintf(tex[k], sizeof tex[k], "ART_F_%d", FIRST_NEW + k);
        snprintf(name[k], sizeof name[k], "ART FACTORY %d", FIRST_NEW + k);
        keys[k] = bhash(tex[k]);
    }
    for (uint32_t i = 0; i < nparts; i++){
        uint32_t p = prt + i * PART_SZ, key = MR32(p);
        if (MR8(p + 4u) != KIND_VINYL) continue;
        if (key == key27 && !p27) p27 = p;
        for (int k = 0; k < NEW_N; k++)
            if (key == keys[k]){ fprintf(stderr, "[vinyls] the car part database has %s already\n", tex[k]); return 0; }
    }
    if (!p27){ fprintf(stderr, "[vinyls] the car part database has no ART_F_27 to follow: no Sha_Do vinyls\n"); return 0; }
    /* ART_F_27's attribute list, and which of its attributes are its own */
    uint32_t list = MR16(p27 + 0xau);
    if (list == 0xffffu || lst + list * 2u + 2u > lst + sub[S_LIST][1]){ fprintf(stderr, "[vinyls] ART_F_27 has no attributes\n"); return 0; }
    int na = (int16_t)MR16(lst + list * 2u);
    if (na <= 0 || na > 16 || lst + list * 2u + 2u + 2u * (uint32_t)na > lst + sub[S_LIST][1]) return 0;
    int own[16], nown = 0;
    for (int j = 0; j < na; j++){
        int ix = (int16_t)MR16(lst + list * 2u + 2u + 2u * (uint32_t)j);
        if (ix < 0 || (uint32_t)ix >= nattr) return 0;
        uint32_t h = MR32(atr + 8u * (uint32_t)ix), v = MR32(atr + 8u * (uint32_t)ix + 4u);
        own[j] = (h == ATTR_KEY && v == key27) ? 1 : h == ATTR_TEXTURE ? 2 : 0;
        nown += own[j] != 0;
    }
    if (nown != 2){ fprintf(stderr, "[vinyls] ART_F_27's attributes are not the game's: no Sha_Do vinyls\n"); return 0; }
    /* what each table gains, kept to its alignment: strings 4 (the next table 8), lists 8 */
    uint32_t g_str = 0;
    for (int k = 0; k < NEW_N; k++) g_str += ((uint32_t)strlen(tex[k]) + 4u) & ~3u, g_str += ((uint32_t)strlen(name[k]) + 4u) & ~3u;
    g_str = (g_str + 7u) & ~7u;
    uint32_t g_lst = ((uint32_t)NEW_N * (2u + 2u * (uint32_t)na) + 7u) & ~7u, g_atr = (uint32_t)NEW_N * 2u * 8u, g_prt = NEW_N * PART_SZ;
    uint32_t gain[7] = { 0, g_str, g_lst, g_atr, 0, 0, g_prt }, nsize = size;
    for (int k = 0; k < 7; k++) nsize += gain[k];
    if ((sub[S_STR][1] + g_str) / 4u > 0xffffu || (sub[S_LIST][1] + g_lst) / 2u > 0xffffu || nattr + 2u * NEW_N > 0x7fffu){
        fprintf(stderr, "[vinyls] the car part database's tables are too full: no Sha_Do vinyls\n"); return 0;
    }
    uint32_t out = gheap_alloc(1, nsize + 8u + 16u, 1);
    if (!out){ fprintf(stderr, "[vinyls] no memory for the car part database's copy\n"); return 0; }
    out = (out + 15u) & ~15u;
    /* the copy: each sub-chunk, then what it gains */
    MR32(out) = CHUNK_PARTS; MR32(out + 4u) = nsize;
    uint32_t o = out + 8u, place[7];
    for (int k = 0; k < n; k++){
        uint32_t sz = sub[k][1] + (k < 7 ? gain[k] : 0u);
        MR32(o) = MR32(sub[k][0]); MR32(o + 4u) = sz;
        if (k == S_PART){                                     /* the parts up to ART_F_27, the new ones, the rest */
            uint32_t before = p27 + PART_SZ - prt;
            memcpy(&MR8(o + 8u), &MR8(prt), before);
            memcpy(&MR8(o + 8u + before + g_prt), &MR8(prt + before), sub[k][1] - before);
        } else memcpy(&MR8(o + 8u), &MR8(sub[k][0] + 8u), sub[k][1]);
        if (k < 7) place[k] = o + 8u;
        o += 8u + sz;
    }
    uint32_t s_at = place[S_STR] + sub[S_STR][1], l_at = place[S_LIST] + sub[S_LIST][1], a_at = place[S_ATTR] + nattr * 8u;
    uint32_t p_at = place[S_PART] + (p27 + PART_SZ - prt);
    /* the attributes past the count: what the table had there (none in the game's) moves after the new ones */
    memmove(&MR8(a_at + g_atr), &MR8(a_at), sub[S_ATTR][1] - nattr * 8u);
    memset(&MR8(a_at), 0, g_atr);
    for (int k = 0; k < NEW_N; k++){
        uint32_t ti = (s_at - place[S_STR]) / 4u;
        memcpy(&MR8(s_at), tex[k], strlen(tex[k]) + 1u); s_at += ((uint32_t)strlen(tex[k]) + 4u) & ~3u;
        uint32_t ni = (s_at - place[S_STR]) / 4u;
        memcpy(&MR8(s_at), name[k], strlen(name[k]) + 1u); s_at += ((uint32_t)strlen(name[k]) + 4u) & ~3u;
        uint32_t li = (l_at - place[S_LIST]) / 2u, a0 = nattr + 2u * (uint32_t)k;
        MR16(l_at) = (uint16_t)na;
        for (int j = 0; j < na; j++){
            uint32_t ix = (uint16_t)MR16(lst + list * 2u + 2u + 2u * (uint32_t)j);
            if (own[j]){
                uint32_t nix = a0 + (own[j] == 1 ? 0u : 1u);
                MR32(place[S_ATTR] + 8u * nix) = MR32(atr + 8u * ix);
                MR32(place[S_ATTR] + 8u * nix + 4u) = own[j] == 1 ? keys[k] : ti;
                ix = nix;
            }
            MR16(l_at + 2u + 2u * (uint32_t)j) = (uint16_t)ix;
        }
        l_at += 2u + 2u * (uint32_t)na;
        memcpy(&MR8(p_at), &MR8(p27), PART_SZ);
        MR32(p_at) = keys[k]; MR16(p_at + 8u) = (uint16_t)ni; MR16(p_at + 0xau) = (uint16_t)li;
        p_at += PART_SZ;
    }
    MR32(place[S_HDR] + 0x20u) = nattr + 2u * NEW_N;
    MR32(place[S_HDR] + 0x38u) = nparts + NEW_N;
    fprintf(stderr, "[vinyls] the car part database: %u parts, %d added after ART_F_27 (ART FACTORY %d to %d, the Japanese "
            "release's; textures from VINYLPACKS)\n", nparts + NEW_N, NEW_N, FIRST_NEW, FIRST_NEW + NEW_N - 1);
    return out;
}

/* the loader, cdecl (chunk): given a copy of the car part database's chunk (a new one each time: the loader turns
 * the copy's model names into pointers) */
static void loader(CPU *c){
    uint32_t chunk = MR32(c->esp + 4u), copy = 0;
    if (ok(chunk, 16u) && MR32(chunk) == CHUNK_PARTS && ok(chunk, MR32(chunk + 4u) + 8u) && fileio_vinyl_packs() > 0)
        copy = grown(chunk);
    if (copy) MR32(c->esp + 4u) = copy;
    sub_00636be0(c);
    if (copy) MR32(c->esp) = chunk;                /* the caller's argument as it was (the loader's ret is behind esp) */
}

void vinylpacks_install(void){
    dispatch_patch(FN_LOADER, loader);
}
