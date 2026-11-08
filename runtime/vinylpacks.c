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
#include <stdlib.h>
#include <string.h>

void dispatch_patch(uint32_t a, void (*f)(CPU *));
void sub_00636be0(CPU *c);                     /* the lifted loader */
uint32_t gheap_alloc(int heap, uint32_t n, int zero);
void gheap_free(int heap, uint32_t p);
static uint32_t g_raw;                         /* grown()'s allocation (its copy is aligned in it) */
static int g_have;                             /* the car part database has the seven */
int fileio_vinyl_packs(void);
uint32_t modpacks_grow(uint32_t chunk);

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
    g_raw = out;
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
    g_have = 1;
    fprintf(stderr, "[vinyls] the car part database: %u parts, %d added after ART_F_27 (ART FACTORY %d to %d, the Japanese "
            "release's; textures from VINYLPACKS)\n", nparts + NEW_N, NEW_N, FIRST_NEW, FIRST_NEW + NEW_N - 1);
    return out;
}

/* runtime/modpacks.c: a profile holds none of the seven (the original game and a page without VINYLPACKS lack
 * them: the car shows no vinyl there, and the next change to it saves none): each goes beside it in MODPARTS.DAT, the
 * profile has the slot's stock part, and a load where the car part database has them puts them back */
int vinylpacks_key(uint32_t key){
    static uint32_t keys[NEW_N];
    if (!keys[0]) for (int k = 0; k < NEW_N; k++){ char t[16]; snprintf(t, sizeof t, "ART_F_%d", FIRST_NEW + k); keys[k] = bhash(t); }
    for (int k = 0; key && k < NEW_N; k++) if (keys[k] == key) return 1;
    return 0;
}
int vinylpacks_have(void){ return g_have; }

/* the loader, cdecl (chunk): given a copy of the car part database's chunk (a new one each time: the loader turns
 * the copy's model names into pointers), with the mod packs' parts too */
static void loader(CPU *c){
    uint32_t chunk = MR32(c->esp + 4u), copy = 0;
    if (ok(chunk, 16u) && MR32(chunk) == CHUNK_PARTS && ok(chunk, MR32(chunk + 4u) + 8u)){
        if (fileio_vinyl_packs() > 0) copy = grown(chunk);
        uint32_t m = modpacks_grow(copy ? copy : chunk);     /* the mods' parts at its end (runtime/modpacks.c) */
        if (m && copy){ gheap_free(1, g_raw); g_raw = 0; }  /* (copied again: the loader keeps that one) */
        if (m) copy = m;
    }
    if (copy) MR32(c->esp + 4u) = copy;
    sub_00636be0(c);
    if (copy) MR32(c->esp) = chunk;                /* the caller's argument as it was (the loader's ret is behind esp) */
}

/* --- the packs' textures' place among the game's ----------------------------------------------------------------
 * A compressed texture pack (a car's VINYLS.BIN) keeps each texture in a block of its own, unpacked when it is wanted:
 * its pixels, palette, TextureInfo and platform info (those two the block's last 0x9C bytes).  The streamer's callback
 * 0x494C10 (cdecl: the unpacked block, its entry with +0xC the unpacked size, the request with +0xBC -> +0x70 the
 * pack) gives the pack the block's TextureInfo (0x48FD30), then hands the block to 0x494A50 as the place of the image
 * offsets [TextureInfo +0x30, + the image and its palette): every texture of the pack whose image offset lies there is
 * pointed into the block (+0x70 = the block + its offset - the place's start) and made (0x5CE170: a P8 one converted
 * to A8R8G8B8, all seven levels).  The block's unload (0x494C80 -> 0x494B10) lets every texture there go.  The game's
 * image offsets are each texture's place in the pack's hash order times 0x55980, a P8 vinyl's size (1786 textures a
 * car: 0 to 0x25526F00), so a block's place holds its own texture alone.  The packs' (tools/vinylpacks.py IMG_BASE
 * until PACK_IMG) were 0x20000000 + 0x56000 each, among the game's: each one's place held a game vinyl too (hash
 * ranks 1532 to 1545), and each one lay in the place of the game vinyl before that (1531 to 1544).  With both
 * loaded, the one that came later took the other into its block 0x1B000 to 0x3A000 bytes in and made it from there,
 * up to 230 KB past the block's end: a car's Art Factory 28 to 34 while another car of its type with one of those
 * vinyls loads.  That drew the vinyl from other memory, or trapped in 0x5CE170 ("memory access out of bounds") with
 * the block near the end of memory (the heap window's top on a 2 GB page; 2026-11-06, three games in a row as
 * friends' cars came), and the block's unload let the other one's texture go.  Here a pack texture's offsets (its
 * name one of theirs, its image offset in their old span) move to places of their own past the game's before the
 * game sees them; the unload reads the same TextureInfo.  A place that holds a texture besides the block's own is
 * logged ("[vinyls] ... takes in ...").  NFSU2_TEST_VINYL_PLACES=1 (script runs only): the packs' offsets as they are. */
#define FN_TEX_IN       0x00494c10u
#define TI_TAIL         0x9cu         /* the TextureInfo and the platform info: a block's last bytes */
#define TI_NAME         0x0cu         /* TextureInfo: its name (24 bytes), +0x24 its hash */
#define TI_IMG          0x30u         /* ... its image offset, +0x34 its palette's (-1: none) */
#define TI_IMG_SZ       0x38u         /* ... the image's size, +0x3C the palette's */
#define OLD_IMG         0x20000000u   /* the packs' first image offset, and the step between theirs, until PACK_IMG */
#define OLD_STEP        0x56000u
#define PACK_IMG        0x30000000u   /* theirs from here on: past every VINYLS.BIN's 1786 x 0x55980 */
#define REQ_PACK        0xbcu         /* the request: -> +0x70 the texture pack (+0x70 -> +0x74 its table of
                                         { hash, TextureInfo }, +0x74 how many) */
void sub_00494c10(CPU *c);                     /* the lifted callback */
static uint32_t g_moved, g_takes;
/* the TextureInfo ti a pack's (named <CAR>_ART_F_28 .. 34, with _MASK or cut short in it) */
static int pack_texture(uint32_t ti){
    char nm[25] = {0};
    for (int i = 0; i < 24; i++) nm[i] = (char)MR8(ti + TI_NAME + (uint32_t)i);
    const char *a = strstr(nm, "_ART_F_");
    if (!a || a[7] < '0' || a[7] > '9' || a[8] < '0' || a[8] > '9') return 0;
    int k = (a[7] - '0') * 10 + (a[8] - '0');
    return k >= FIRST_NEW && k < FIRST_NEW + NEW_N && (!a[9] || a[9] == '_');
}
static void tex_in(CPU *c){
    static int keep = -1, packs = -1;
    if (keep < 0){ const char *s = getenv("NFSU2_INPUT_SCRIPT"), *e = getenv("NFSU2_TEST_VINYL_PLACES"); keep = s && *s && e && *e == '1'; }
    if (packs < 0) packs = fileio_vinyl_packs() > 0;
    uint32_t blk = MR32(c->esp + 4u), ent = MR32(c->esp + 8u), req = MR32(c->esp + 12u);
    uint32_t n = packs && ok(ent, 0x10u) ? MR32(ent + 0xcu) : 0u, ti = blk + n - TI_TAIL;
    if (blk && n >= TI_TAIL && ok(blk, n)){
        uint32_t img = MR32(ti + TI_IMG), pal = MR32(ti + TI_IMG + 4u);
        if (!keep && img >= OLD_IMG && img < OLD_IMG + 2u * NEW_N * OLD_STEP && pack_texture(ti)){
            MR32(ti + TI_IMG) = img = PACK_IMG + (img - OLD_IMG);
            if (pal != 0xffffffffu) MR32(ti + TI_IMG + 4u) = pal = PACK_IMG + (pal - OLD_IMG);
            if (!g_moved++) fprintf(stderr, "[vinyls] the packs' textures at places of their own, past the game's\n");
        }
        /* the place 0x494C10 gives 0x494A50: the textures in it besides the block's own */
        uint32_t len = pal != 0xffffffffu ? MR32(ti + TI_IMG_SZ + 4u) + pal - img : MR32(ti + TI_IMG_SZ);
        uint32_t hold = ok(req, REQ_PACK + 4u) ? MR32(req + REQ_PACK) : 0u, pack = ok(hold, 0x74u) ? MR32(hold + 0x70u) : 0u;
        uint32_t list = ok(pack, 0x78u) ? MR32(pack + 0x70u) : 0u, tab = ok(list, 0x78u) ? MR32(list + 0x74u) : 0u;
        uint32_t cnt = tab ? MR32(pack + 0x74u) : 0u, h = MR32(ti + 0x24u);
        for (uint32_t i = 0; i < cnt && i < 0x10000u && ok(tab + i * 8u, 8u); i++){
            uint32_t o = MR32(tab + i * 8u + 4u);
            if (!o || o == ti || !ok(o, 0x7cu) || MR32(o + 0x24u) == h || !MR32(o + TI_IMG_SZ)) continue;
            uint32_t at = MR32(o + TI_IMG);
            if ((int32_t)at < (int32_t)img || (int32_t)at >= (int32_t)(img + len)) continue;
            if (g_takes++ < 8){
                char a[25] = {0}, b[25] = {0};
                for (int k = 0; k < 24; k++){ a[k] = (char)MR8(ti + TI_NAME + (uint32_t)k); b[k] = (char)MR8(o + TI_NAME + (uint32_t)k); }
                uint32_t end = at - img + MR32(o + TI_IMG_SZ);
                fprintf(stderr, "[vinyls] %s's block takes in %s: its image 0x%x bytes in, %u bytes past the block's end\n",
                        a, b, at - img, end > n ? end - n : 0u);
            }
        }
    }
    sub_00494c10(c);
}

void vinylpacks_install(void){
    dispatch_patch(FN_LOADER, loader);
    dispatch_patch(FN_TEX_IN, tex_in);
}
