/* modparts_unit.c -- tests for runtime/modpacks.c's MODPARTS.DAT (what a profile never holds, kept beside it), without
 * the game: Art Factory 28-34 (the Japanese release's vinyls), mod rims, and the cars of a car type the game has not.
 *
 *   clang -O1 -g -Wall -Wno-unused-function -Iruntime -o out/modparts_unit tests/modparts_unit.c && out/modparts_unit
 *
 * The game's memory is a 4 GB reservation of our own with only the pages a test uses marked committed.  garage.c's
 * list of cars is two career cars (slot 1, slot 2) and a Customize car; a save here is what garage.c's 0x52A450 does
 * (the payload with the stand-ins, MODPARTS.NEW, a payload as it is when that fails, then the profile written); the
 * game's calls modpacks.c makes (a car type's info, a ride's Init and stock parts) are stand-ins: car types PEUGOT (0)
 * and FOCUS (1), every slot's stock part a key of its own but slot 0x40 (the vinyl: none).  One mod rim (BLITZ, its
 * look-alike AVUS) stands for MODPACKS.  MODPARTS.DAT goes to a folder of the test's own.  Cases:
 *   af-hide        a career car with ART_F_28 on: the payload is made with the slot's stock part (none), the car has
 *                  ART_F_28 again after, and MODPARTS.DAT holds it (kind 0: handle, car type, slot 0x40, the key, 0),
 *                  after its head (kind 3: the profile's name, write 1)
 *   af-back        the profile loaded (the stock part in the slot): ART_F_28 back on the car
 *   af-wait        loaded where the car part database lacks them (no VINYLPACKS): the car keeps none, and the next
 *                  save keeps ART_F_28 in the file all the same
 *   af-elsewhere   the car changed elsewhere (another vinyl in the slot): it keeps that one, ART_F_28 not kept
 *   rawback        a car of a car type the game has not, the stand-in here (garage.c addon_cars): every payload has
 *                  it as it was loaded (its own car type and 170 keys), the stand-in after; no MODPARTS.DAT for it;
 *                  the load itself nothing to save
 *   trade          two such cars (ADDA in slot 1, ADDB in slot 2), Trade Car on slot 1 (garage.c trade(): slot 2's
 *                  record into slot 1's): the payload has ADDB in slot 1, slot 2 empty as the game made it
 *   changed        a part bought on the stand-in: the payload has the stand-in with that part (the player's car
 *                  now), at that save and the next
 *   trade-in       the car lot's trade-in (garage.c's 0x516B90 hook) or a new tuned car (0x52A710) in its record: a
 *                  stock car of the stand-in's car type there, untouched: the payload has that stock car
 *   deleted-tuned  Delete Tuned Car (the record not in use): the payload has the record as the game left it
 *   kinds          entries of kinds this page does not know (a later page's kind 9; kinds 1 and 2 of a build that
 *                  never shipped) are written again as they were
 *   writefail      (M-A) MODPARTS.NEW cannot be made (a full disk): the payload made again has the raw ART_F_28 on
 *                  slot 1's car and the waiting ART_F_30 (no VINYLPACKS) in its slot on the Customize car, as a page
 *                  of before; MODPARTS.DAT as it was; the stand-ins after; the next save that works keeps them
 *   counter        a vinyl changed for another (ART_F_28 to ART_F_29: the car's bytes in the profile the same), the
 *                  profile written, the rename cut: the load takes MODPARTS.NEW by its write counter (ART_F_29 on the
 *                  car) and renames it; a lone MODPARTS.NEW of another profile is not taken; a MODPARTS.DAT a page of
 *                  before wrote last (its head word 0, our head inside) wins over a MODPARTS.NEW of ours
 *   kind0-unknown  a part kept beside the profile (ART_F_28) on a car of a car type the game has not: not put on it
 *                  (the car stays as loaded), written again with it
 *   names          a profile named with a space has its file
 *   foreign        a MODPARTS.DAT of another version: never written over; a second one: kept as .OLD2 beside the
 *                  first .OLD
 *   deleted        the profile deleted: a new one of the same name in this session gets nothing of it
 *   cut-before-profile  MODPARTS.NEW of a save written, the profile not: the load takes MODPARTS.DAT (MODPARTS.NEW's
 *                  head names another payload's cars) and removes that MODPARTS.NEW; the next save keeps the part
 *   new-profile    a profile made this session counts its writes from its own files; a cut rename is recovered
 *   modded-keys    a modded car whose own keys hold a mod's rim and ART_F_28: the payload has them as loaded, and the
 *                  game's "anything to save?" says no while nothing changed
 * Every case prints "PASS <case> ..." or "FAIL <case> ..."; exit 1 on a FAIL. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../runtime/cpu.h"
uint8_t *g_mem;
uint8_t g_cbits[1u << 15];
static void commit(uint32_t a, uint32_t n){ for (uint32_t pg = a >> 14; pg <= (a + n - 1) >> 14; pg++) g_cbits[pg >> 3] |= (uint8_t)(1u << (pg & 7)); }

static int g_fail;
#define CHECK(cond, ...) do{ if (cond) printf("PASS " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); g_fail = 1; } \
                             printf("\n"); fflush(stdout); }while(0)

/* the stand-ins of what modpacks.c needs */
static uint32_t g_heap = 0x20000000u;
uint32_t gheap_alloc(int heap, uint32_t n, int zero){ (void)heap; uint32_t a = g_heap; g_heap += (n + 15u) & ~15u; commit(a, n); if (zero) memset(g_mem + a, 0, n); return a; }
int fileio_mod_pack(const char *rel, char *out, size_t outsz){ (void)rel; (void)out; (void)outsz; return 0; }   /* (the rim: below) */
uint32_t language_add_text(const char *name, const char *text){ (void)name; (void)text; return 0; }
uint32_t language_text(uint32_t hash){ (void)hash; return 0; }
void sub_005463b0_orig(CPU *c){ (void)c; }
void sub_0050e960_orig(CPU *c){ (void)c; }
void sub_005f04d0_orig(CPU *c){ (void)c; }
static char g_dir[512];
/* as fileio.c's: the path in the save folder, 1 only if the file is there (a file to write: its path all the same) */
int fileio_resolve_save(const char *win, char *out, size_t outsz){
    const char *p = strchr(win, '\\');
    if (!p) return 0;
    snprintf(out, outsz, "%s/%s", g_dir, p + 1);
    for (char *q = out + strlen(g_dir); *q; q++) if (*q == '\\') *q = '/';
    struct stat st;
    return stat(out, &st) == 0;
}
int fileio_resolve_save_w(const char *win, char *out, size_t outsz){ return fileio_resolve_save(win, out, outsz); }
/* vinylpacks.c's (its own file has statics of the same names as modpacks.c's) */
static uint32_t hash_of(const char *s){ uint32_t h = 0xffffffffu; for (; *s; s++) h = h * 33u + (uint8_t)*s; return h; }
static int g_vp_have = 1;
int vinylpacks_key(uint32_t key){
    for (int k = 28; key && k <= 34; k++){ char t[16]; snprintf(t, sizeof t, "ART_F_%d", k); if (hash_of(t) == key) return 1; }
    return 0;
}
int vinylpacks_have(void){ return g_vp_have; }
/* garage.c's list: slot 1's car, slot 2's, then the Customize car; a slot's car is in use with +0x7F5, a Customize
 * car's with +0x10 */
#define DB      0x0083ad90u
#define CAR     0x10000000u
#define CAR2    0x10003000u
#define CUST    0x10001000u
int garage_records(uint32_t db, uint32_t *out, int max){
    (void)db; int n = 0;
    if (n < max) out[n++] = CAR;
    if (n < max) out[n++] = CAR2;
    if (n < max) out[n++] = CUST;
    return n;
}
int garage_car_in_use(uint32_t db, uint32_t r){ (void)db; return r == CUST ? MR32(r + 0x10u) != 0 : MR8(r + 0x7f5u) != 0; }
/* the game's car types (CarTypeInfo[46]) and parts */
#define TYPES   0x10100000u
#define PARTS   0x10200000u                 /* a stock part: its key at +0 */
#define TYPE_SZ 0x890u
static uint32_t stock_of(uint32_t type_ix, uint32_t slot){ return slot == 0x40u ? 0u : 0x51000000u | type_ix << 12 | slot; }
void dispatch_indirect(CPU *c, uint32_t addr){
    uint32_t a0 = MR32(c->esp + 4u);
    if (addr == 0x00610130u){                                    /* a car type's info */
        c->eax = 0;
        for (uint32_t i = 0; i < 46u; i++) if (MR32(TYPES + i * TYPE_SZ + 0xd0u) == a0) c->eax = TYPES + i * TYPE_SZ;
    } else if (addr == 0x00610270u) MR32(c->ecx) = a0;            /* a ride's Init: its car type */
    else if (addr == 0x00637040u){                               /* its stock parts */
        uint32_t t = MR32(c->ecx);
        for (uint32_t s = 0; s < 0xaau; s++){
            uint32_t k = stock_of(t, s), p = PARTS + (t * 0xaau + s) * 4u;
            MR32(p) = k;
            MR32(c->ecx + 0x590u + 4u * s) = k ? p : 0;
        }
    }
}
#include "../runtime/modpacks.c"

#define R_TYPE  (0x18u + 8u)
#define R_KEY(s) (0x18u + 0xcu + 4u * (s))
#define SLOT_RIM 0x4au
static uint32_t PEUGOT, FOCUS, ADDON, ADDA, ADDB, AF28, AF29, AF30, AF12, BLITZ, AVUS, TUNED0, SLOT1, SLOT2;
static CPU g_c;
static void car_init(uint32_t r, uint32_t handle, uint32_t type_hash, uint32_t type_ix){
    memset(g_mem + r, 0, 0x7f8);
    MR32(r + 8u) = handle; MR32(r + R_TYPE) = type_hash; MR32(r + 0x10u) = 1; MR8(r + 0x7f5u) = 1;
    for (uint32_t s = 0; s < 0xaau; s++) MR32(r + R_KEY(s)) = stock_of(type_ix, s);
    MR32(r + 0x300u) = 0x77777777u;                              /* (its tuning, performance: bytes of its own) */
}
/* a car of a car type the game has not, keys as given, in the record r; then garage.c addon_cars' conversion */
static void addon(uint32_t r, uint32_t handle, uint32_t type, const uint32_t *keys, uint32_t size){
    car_init(r, handle, type, 0);
    for (uint32_t s = 0; s < 0xaau; s++) MR32(r + R_KEY(s)) = keys[s];
}
static void convert(uint32_t r, uint32_t size){
    static uint32_t was[0xaa], now[0xaa];
    uint32_t orig = MR32(r + R_TYPE);
    for (uint32_t s = 0; s < 0xaau; s++) was[s] = MR32(r + R_KEY(s));
    modparts_stock_keys(&g_c, PEUGOT, now);
    MR32(r + R_TYPE) = PEUGOT;
    for (uint32_t s = 0; s < 0xaau; s++) MR32(r + R_KEY(s)) = now[s];
    modparts_track(0, r, size, orig, was, r == CUST ? MR32(r + 0x10u) != 0 : MR8(r + 0x7f5u) != 0);
}
/* a record as a payload has it: its car type and keys at the time */
typedef struct { uint32_t type, keys[0xaa]; } Rec;
static void rec_of(uint32_t r, Rec *x){ x->type = MR32(r + R_TYPE); for (uint32_t s = 0; s < 0xaau; s++) x->keys[s] = MR32(r + R_KEY(s)); }
static int rec_is(const Rec *x, uint32_t type, const uint32_t *keys){ return x->type == type && !memcmp(x->keys, keys, sizeof x->keys); }
/* a save of player 1's profile NAME, as garage.c's 0x52A450 makes it: the payload with the stand-ins (fp its cars'
 * fingerprint), MODPARTS.NEW, the payload as it is if that fails (raw_fp), the profile written.  What the payloads
 * had: g_pay (slot 1's car, slot 2's, the Customize car), g_raw_pay */
static Rec g_pay[3], g_raw_pay[3];
static int g_raw;
static void seen(Rec *x){ rec_of(CAR, &x[0]); rec_of(CAR2, &x[1]); rec_of(CUST, &x[2]); }
static void save_as(const char *name, uint64_t fp, uint64_t raw_fp, int write){
    modparts_hide(&g_c, DB, 0);
    seen(g_pay);
    modparts_built(0, 0, fp, name, 0);
    g_raw = !modparts_stage(0, fp, name);
    if (g_raw){ modparts_raw(&g_c, DB, 0, 1); seen(g_raw_pay); modparts_raw(&g_c, DB, 0, 0); }
    char path[700]; snprintf(path, sizeof path, "%s/%s/%s", g_dir, name, name);
    if (write) modparts_written(path, g_raw ? raw_fp : fp, name);
}
static void save(const char *name, uint64_t fp){ save_as(name, fp, fp ^ 0xfeed, 1); }
static void kept_payload(const char *name, uint64_t fp){ modparts_hide(&g_c, DB, 0); modparts_built(0, 0, fp, name, 1); }
static void profile_load(const char *name, uint64_t fp){ modparts_loaded(&g_c, DB, 0, 0, fp, name, 0); }
typedef struct { uint32_t w[8]; } Ent;
static uint32_t g_hd3, g_headname, g_headcount;
/* a MODPARTS file's entries but its head (g_hd3 its head word, g_headname/count its head entry's) */
static int read_file(const char *name, const char *file, Ent *e, int max){
    char path[700]; snprintf(path, sizeof path, "%s/%s/%s", g_dir, name, file);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint32_t hd[4];
    int n = -2;
    g_headname = g_headcount = 0;
    if (fread(hd, sizeof hd, 1, f) == 1 && hd[0] == 0x504d324eu && hd[1] == 1u){
        n = 0; g_hd3 = hd[3];
        while (n < max && fread(e[n].w, sizeof e[n].w, 1, f) == 1){
            if (e[n].w[7] == 3 && e[n].w[0] == 0x4448504du){ g_headname = e[n].w[1]; g_headcount = e[n].w[2]; continue; }
            n++;
        }
    }
    fclose(f);
    return n;
}
static int count_kind(const Ent *e, int n, uint32_t kind){ int k = 0; for (int i = 0; i < n; i++) k += e[i].w[7] == kind; return k; }
static void rm(const char *name, const char *file){ char p[700]; snprintf(p, sizeof p, "%s/%s/%s", g_dir, name, file); remove(p); }
static void clean(const char *name){ rm(name, "MODPARTS.DAT"); rm(name, "MODPARTS.NEW"); rm(name, "MODPARTS.OLD"); rm(name, "MODPARTS.OLD2"); }
static int there(const char *name, const char *file){ char p[700]; snprintf(p, sizeof p, "%s/%s/%s", g_dir, name, file); struct stat st; return stat(p, &st) == 0; }
/* a fresh start: no file, the profile loaded with nothing beside it */
static void fresh_load(const char *name, uint64_t fp){ clean(name); profile_load(name, fp); }
static void write_raw(const char *name, const char *file, const uint32_t *hd, const uint32_t *ent, int n){
    char p[700]; snprintf(p, sizeof p, "%s/%s/%s", g_dir, name, file);
    FILE *w = fopen(p, "wb"); fwrite(hd, 16, 1, w); fwrite(ent, 32, (size_t)n, w); fclose(w);
}
static uint32_t first_word(const char *name, const char *file){
    char p[700]; snprintf(p, sizeof p, "%s/%s/%s", g_dir, name, file);
    uint32_t b[12] = { 0 }; FILE *x = fopen(p, "rb");
    if (x){ if (fread(b, sizeof b, 1, x) != 1) b[4] = 0; fclose(x); }
    return b[4];
}

int main(void){
    g_mem = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_mem == MAP_FAILED){ perror("mmap"); return 2; }
    snprintf(g_dir, sizeof g_dir, "%s/modparts_unit.%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    mkdir(g_dir, 0755);
    char d[700];
    snprintf(d, sizeof d, "%s/ANN", g_dir); mkdir(d, 0755);
    snprintf(d, sizeof d, "%s/MY CAR", g_dir); mkdir(d, 0755);
    commit(0x00130000u - 0x8000u, 0x10000u); g_c.esp = 0x00130000u;
    commit(0x008a1cccu, 4); commit(CAR, 0x4000); commit(TYPES, 46u * TYPE_SZ); commit(PARTS, 46u * 0xaau * 4u);
    PEUGOT = hash_of("PEUGOT"); FOCUS = hash_of("FOCUS"); ADDON = hash_of("ADDONCAR"); ADDA = hash_of("ADDA"); ADDB = hash_of("ADDB");
    AF28 = hash_of("ART_F_28"); AF29 = hash_of("ART_F_29"); AF30 = hash_of("ART_F_30"); AF12 = hash_of("ART_F_12");
    BLITZ = hash_of("BLITZ_BRW03"); AVUS = hash_of("AVUS_AF502"); TUNED0 = hash_of("TUNED_CAR_0"); SLOT1 = 0x319a8ca1u; SLOT2 = 0x319a8ca2u;
    MR32(0x008a1cccu) = TYPES;
    for (uint32_t i = 0; i < 46u; i++){ MR32(TYPES + i * TYPE_SZ + 0xd0u) = 0x7e570000u + i; MR32(TYPES + i * TYPE_SZ + 0x840u) = i; }
    MR32(TYPES + 0xd0u) = PEUGOT; MR32(TYPES + TYPE_SZ + 0xd0u) = FOCUS;
    /* MODPACKS: one rim of a mod (its look-alike a rim of the game's) */
    g.p = calloc(1, sizeof *g.p); g.nparts = 1; g.p[0].key = BLITZ; g.p[0].stand = AVUS; g_grown = 1;
    car_init(CAR, SLOT1, PEUGOT, 0); car_init(CAR2, SLOT2, PEUGOT, 0); MR8(CAR2 + 0x7f5u) = 0;   /* (slot 2: no car yet) */
    car_init(CUST, TUNED0, FOCUS, 1);
    Ent e[1100]; int n;
    static uint32_t own[0xaa], ownb[0xaa], stockp[0xaa];
    for (uint32_t s = 0; s < 0xaau; s++){ own[s] = s >= 0x60u && s < 0x69u ? 0 : 0xadd00000u | s; ownb[s] = 0xbbb00000u | s; stockp[s] = stock_of(0, s); }

    /* af-hide */
    profile_load("ANN", 0x1111);
    MR32(CAR + R_KEY(0x40u)) = AF28;
    save("ANN", 0x2222);
    n = read_file("ANN", "MODPARTS.DAT", e, 1100);
    CHECK(g_pay[0].keys[0x40] == 0 && MR32(CAR + R_KEY(0x40u)) == AF28 && n == 1 && e[0].w[0] == SLOT1 && e[0].w[1] == PEUGOT &&
          e[0].w[2] == 0x40u && e[0].w[3] == AF28 && e[0].w[4] == 0 && e[0].w[7] == 0 && g_hd3 == 1u && g_headcount == 1u && g_headname == name_hash("ANN"),
          "af-hide the payload had %08x in the vinyl slot, the car %08x after; MODPARTS.DAT %d entr%s, write %u (head %u), its profile %s",
          g_pay[0].keys[0x40], MR32(CAR + R_KEY(0x40u)), n, n == 1 ? "y" : "ies", g_hd3, g_headcount, g_headname == name_hash("ANN") ? "ANN" : "?");

    /* af-back */
    MR32(CAR + R_KEY(0x40u)) = 0;                                /* as the profile holds it */
    profile_load("ANN", 0x2222);
    CHECK(MR32(CAR + R_KEY(0x40u)) == AF28, "af-back the vinyl slot %08x", MR32(CAR + R_KEY(0x40u)));

    /* af-wait */
    MR32(CAR + R_KEY(0x40u)) = 0; g_vp_have = 0;
    profile_load("ANN", 0x2222);
    uint32_t waited = MR32(CAR + R_KEY(0x40u));
    save("ANN", 0x3333);
    n = read_file("ANN", "MODPARTS.DAT", e, 1100);
    CHECK(waited == 0 && n == 1 && e[0].w[3] == AF28 && g_hd3 == 2u, "af-wait the slot %08x meanwhile; kept again: %d entr%s, write %u", waited, n,
          n == 1 ? "y" : "ies", g_hd3);
    g_vp_have = 1;

    /* af-elsewhere */
    MR32(CAR + R_KEY(0x40u)) = AF12;                             /* the PC game gave it Art Factory 12 */
    profile_load("ANN", 0x3333);
    uint32_t kept = MR32(CAR + R_KEY(0x40u));
    save("ANN", 0x4444);
    n = read_file("ANN", "MODPARTS.DAT", e, 1100);
    CHECK(kept == AF12 && n == -1, "af-elsewhere the slot %08x; MODPARTS.DAT %s", kept, n == -1 ? "gone (nothing to keep)" : "still there");
    MR32(CAR + R_KEY(0x40u)) = 0;

    /* rawback: the Customize car of a car type the game has not */
    fresh_load("ANN", 0x5000);
    addon(CUST, TUNED0, ADDON, own, 0x434u); convert(CUST, 0x434u);
    kept_payload("ANN", 0x5000);                                 /* (the game keeps the payload it compares with) */
    int asked = modparts_changed(&g_c, 0);
    save("ANN", 0x5555);
    int raw1 = rec_is(&g_pay[2], ADDON, own), stand1 = MR32(CUST + R_TYPE) == PEUGOT && MR32(CUST + R_KEY(0)) == stockp[0];
    save("ANN", 0x5556);
    int raw2 = rec_is(&g_pay[2], ADDON, own);
    n = read_file("ANN", "MODPARTS.DAT", e, 1100);
    CHECK(raw1 && raw2 && stand1 && !asked && n == -1, "rawback the payload has the car as it was: %d (and at the next save %d), the stand-in "
          "after: %d; the load itself something to save: %d; MODPARTS.DAT %s", raw1, raw2, stand1, asked, n == -1 ? "none" : "MADE");
    g_mk.on = 0;

    /* trade: ADDA in slot 1, ADDB in slot 2, both the stand-in */
    fresh_load("ANN", 0x8000);
    addon(CAR, SLOT1, ADDA, own, 0x7f8u); convert(CAR, 0x7f8u);
    addon(CAR2, SLOT2, ADDB, ownb, 0x7f8u); convert(CAR2, 0x7f8u);
    car_init(CUST, TUNED0, FOCUS, 1);
    save("ANN", 0x8001);
    int both = rec_is(&g_pay[0], ADDA, own) && rec_is(&g_pay[1], ADDB, ownb);
    /* garage.c trade(): slot 1 traded, the last car (slot 2's) moves into slot 1's record, slot 2 empty */
    modparts_car_gone(0, CAR, SLOT1);
    for (uint32_t o = 4u; o < 0x7f8u; o += 4u) if (o != 8u) MR32(CAR + o) = MR32(CAR2 + o);
    memset(g_mem + CAR2 + 0x18u, 0, 0x7e0); MR8(CAR2 + 0x7f5u) = 0;
    modparts_car_moved(0, CAR2, SLOT2, CAR, SLOT1);
    save("ANN", 0x8002);
    CHECK(both && rec_is(&g_pay[0], ADDB, ownb) && g_pay[1].type == 0, "trade before: both as they were %d; after Trade Car: slot 1 %s, slot 2 %s",
          both, rec_is(&g_pay[0], ADDB, ownb) ? "ADDB as it was" : g_pay[0].type == ADDA ? "ADDA (the car sold)" : "the stand-in (ADDB LOST)",
          g_pay[1].type == 0 ? "empty" : "NOT empty");

    /* changed: a part bought on the stand-in */
    fresh_load("ANN", 0x9000);
    addon(CAR, SLOT1, ADDA, own, 0x7f8u); convert(CAR, 0x7f8u);
    MR32(CAR + R_KEY(0x30u)) = 0x12340030u;                      /* the shop's buy */
    static uint32_t bought[0xaa]; memcpy(bought, stockp, sizeof bought); bought[0x30] = 0x12340030u;
    save("ANN", 0x9001);
    int c1 = rec_is(&g_pay[0], PEUGOT, bought);
    MR32(CAR + R_KEY(0x30u)) = stockp[0x30];                     /* (even back to the stock part: the player's car now) */
    save("ANN", 0x9002);
    CHECK(c1 && g_pay[0].type == PEUGOT, "changed the payload has the stand-in with the part bought: %d; the next save, the part taken off "
          "again: %s", c1, g_pay[0].type == PEUGOT ? "the stand-in still" : "the old car AGAIN");
    /* ... and a tuning change alone (bytes past the keys) */
    fresh_load("ANN", 0x9100);
    addon(CAR, SLOT1, ADDA, own, 0x7f8u); convert(CAR, 0x7f8u);
    MR32(CAR + 0x348u) ^= 0x01010101u;                           /* a setup of its tuning */
    save("ANN", 0x9101);
    CHECK(g_pay[0].type == PEUGOT, "changed a tuning change alone: the payload has %s", g_pay[0].type == PEUGOT ? "the stand-in, tuned" : "the old car");

    /* trade-in: the car lot's trade-in of the car (0x516B90 there: garage.c), a stock PEUGOT in its record */
    fresh_load("ANN", 0xa000);
    addon(CAR, SLOT1, ADDA, own, 0x7f8u); convert(CAR, 0x7f8u);
    modparts_car_gone(0, CAR, SLOT1);                            /* (the hook) */
    car_init(CAR, SLOT1, PEUGOT, 0);
    save("ANN", 0xa001);
    CHECK(rec_is(&g_pay[0], PEUGOT, stockp), "trade-in the payload has %s", rec_is(&g_pay[0], PEUGOT, stockp) ? "the stock car bought" : "the OLD car");

    /* deleted-tuned: Delete Tuned Car (the record not in use) */
    fresh_load("ANN", 0xa100);
    addon(CUST, TUNED0, ADDON, own, 0x434u); convert(CUST, 0x434u);
    MR32(CUST + 0x10u) = 0;
    save("ANN", 0xa101);
    CHECK(g_pay[2].type == PEUGOT, "deleted-tuned the payload has the record as the game left it: %s", g_pay[2].type == PEUGOT ? "yes" : "the OLD car");
    car_init(CUST, TUNED0, FOCUS, 1);

    /* kinds: a later page's kind 9, and kinds 1 and 2 (never shipped), in MODPARTS.DAT */
    clean("ANN");
    car_init(CAR, SLOT1, PEUGOT, 0);
    { uint32_t hd[4] = { 0x504d324eu, 1u, 3u, 0 }, en[24] = { 0xAAAA, 1, 2, 3, 4, 5, 6, 9,  SLOT1, PEUGOT, 0, ADDA, 1, 7, 8, 1,  SLOT1, 0, 1, 2, 3, 4, 5, 2 };
      write_raw("ANN", "MODPARTS.DAT", hd, en, 3); }
    profile_load("ANN", 0xb000);
    save("ANN", 0xb001);
    n = read_file("ANN", "MODPARTS.DAT", e, 1100);
    CHECK(n == 3 && count_kind(e, n, 9) == 1 && count_kind(e, n, 1) == 1 && count_kind(e, n, 2) == 1 && e[0].w[0] == 0xAAAA,
          "kinds written again as they were: %d entries (kinds 9, 1, 2: %d %d %d)", n, count_kind(e, n, 9), count_kind(e, n, 1), count_kind(e, n, 2));

    /* writefail (M-A): MODPARTS.NEW cannot be made, a raw part on slot 1's car and one waiting on the Customize car */
    clean("ANN");
    car_init(CAR, SLOT1, PEUGOT, 0); car_init(CUST, TUNED0, FOCUS, 1);
    MR32(CUST + R_KEY(0x40u)) = 0;
    { uint64_t fpc = car_fp(CUST); uint32_t hd[4] = { 0x504d324eu, 1u, 1u, 0 }, en[8] = { TUNED0, FOCUS, 0x40u, AF30, 0, (uint32_t)fpc, (uint32_t)(fpc >> 32), 0 };
      write_raw("ANN", "MODPARTS.DAT", hd, en, 1); }
    g_vp_have = 0;
    profile_load("ANN", 0xc000);                                 /* ART_F_30 waits (no VINYLPACKS) */
    MR32(CAR + R_KEY(0x40u)) = AF28;                             /* bought here (a profile of before, or VINYLPACKS gone since) */
    int n_before = read_file("ANN", "MODPARTS.DAT", e, 1100);
    snprintf(d, sizeof d, "%s/ANN/MODPARTS.NEW", g_dir); mkdir(d, 0755);   /* fopen(.NEW, "wb") fails */
    save("ANN", 0xc001);
    rmdir(d);
    n = read_file("ANN", "MODPARTS.DAT", e, 1100);
    int raw_ok = g_raw && g_raw_pay[0].keys[0x40] == AF28 && g_raw_pay[2].keys[0x40] == AF30;
    int after = MR32(CAR + R_KEY(0x40u)) == AF28 && MR32(CUST + R_KEY(0x40u)) == 0;
    int as_was = n == n_before && e[0].w[3] == AF30;
    save("ANN", 0xc002);
    int nn = read_file("ANN", "MODPARTS.DAT", e, 1100), both_kept = 0;
    for (int i = 0; i < nn; i++) both_kept += e[i].w[7] == 0 && (e[i].w[3] == AF28 || e[i].w[3] == AF30);
    CHECK(raw_ok && after && as_was && both_kept == 2, "writefail the payload made again: slot 1's vinyl %08x (%s), the Customize car's %08x "
          "(%s); after it as before: %d; MODPARTS.DAT as it was: %d; the next save keeps both: %d", g_raw_pay[0].keys[0x40],
          g_raw_pay[0].keys[0x40] == AF28 ? "ART_F_28 raw" : "NOT", g_raw_pay[2].keys[0x40], g_raw_pay[2].keys[0x40] == AF30 ? "the waiting ART_F_30" : "LOST",
          after, as_was, both_kept);
    g_vp_have = 1; MR32(CAR + R_KEY(0x40u)) = 0;

    /* counter: ART_F_28 for ART_F_29, the rename cut */
    clean("ANN");
    car_init(CAR, SLOT1, PEUGOT, 0); MR32(CAR + R_KEY(0x40u)) = AF28;
    profile_load("ANN", 0xd000);
    save("ANN", 0xd001);                                         /* MODPARTS.DAT: ART_F_28, write k */
    read_file("ANN", "MODPARTS.DAT", e, 1100); uint32_t wk = g_hd3;
    MR32(CAR + R_KEY(0x40u)) = AF29;
    save_as("ANN", 0xd001, 0, 0);                                /* MODPARTS.NEW written, the profile too (the same bytes), no rename */
    int both_there = there("ANN", "MODPARTS.DAT") && there("ANN", "MODPARTS.NEW");
    MR32(CAR + R_KEY(0x40u)) = 0;                                /* as the profile holds it */
    profile_load("ANN", 0xd001);
    int got29 = MR32(CAR + R_KEY(0x40u)) == AF29;
    n = read_file("ANN", "MODPARTS.DAT", e, 1100);
    int renamed = !there("ANN", "MODPARTS.NEW") && n == 1 && e[0].w[3] == AF29 && g_hd3 == wk + 1u;
    /* a lone MODPARTS.NEW of another profile (its head names BOB) */
    clean("ANN");
    { uint32_t hd[4] = { 0x504d324eu, 1u, 2u, 5u }, en[16] = { 0x4448504du, name_hash("BOB"), 5, 0, 0, 0, 0, 3,  SLOT1, PEUGOT, 0x40u, AF30, 0, 0, 0, 0 };
      write_raw("ANN", "MODPARTS.NEW", hd, en, 2); }
    MR32(CAR + R_KEY(0x40u)) = 0;
    profile_load("ANN", 0xd002);
    int lone_left = g_wait[0].n == 0 && MR32(CAR + R_KEY(0x40u)) == 0 && there("ANN", "MODPARTS.NEW");
    /* a MODPARTS.DAT of a page of before (head word 0, our head carried inside) beside a MODPARTS.NEW of ours: the NEW
     * came after it (such a page writes through MODPARTS.NEW and renames it, so ours could not have lived through its save):
     * ours is taken (a cut rename after an older page's save, AF28 swapped for AF29: the same cars) */
    clean("ANN");
    { uint64_t fpc = car_fp(CAR);
      uint32_t hd[4] = { 0x504d324eu, 1u, 2u, 0 }, en[16] = { 0x4448504du, name_hash("ANN"), 3, 0xd003, 0, 0, 0, 3,  SLOT1, PEUGOT, 0x40u, AF12 ^ 1u, 0, (uint32_t)fpc, (uint32_t)(fpc >> 32), 0 };
      write_raw("ANN", "MODPARTS.DAT", hd, en, 2);
      uint32_t hn[4] = { 0x504d324eu, 1u, 2u, 4u }, eo[16] = { 0x4448504du, name_hash("ANN"), 4, 0xd003, 0, 0, 0, 3,  SLOT1, PEUGOT, 0x40u, AF29, 0, (uint32_t)fpc, (uint32_t)(fpc >> 32), 0 };
      write_raw("ANN", "MODPARTS.NEW", hn, eo, 2); }
    g_vp_have = 0;
    profile_load("ANN", 0xd003);
    int new_wins = g_wait[0].n == 1 && g_wait[0].e[0].w[3] == AF29;
    g_vp_have = 1;
    CHECK(both_there && got29 && renamed && lone_left && new_wins, "counter the cut rename: ART_F_29 on the car %d, MODPARTS.NEW renamed at the "
          "load %d; a lone MODPARTS.NEW of another profile not taken %d; ours over a page of before's MODPARTS.DAT %d", got29, renamed, lone_left, new_wins);
    clean("ANN"); MR32(CAR + R_KEY(0x40u)) = 0;

    /* kind0-unknown: a part this game lacks (ART_F_28, no VINYLPACKS) waiting on a car of a car type it lacks */
    clean("ANN");
    addon(CUST, TUNED0, ADDON, own, 0x434u); MR32(CUST + R_KEY(0x40u)) = 0;
    { uint64_t cfp = car_fp(CUST); uint32_t hd[4] = { 0x504d324eu, 1u, 1u, 0 }, en[8] = { TUNED0, ADDON, 0x40u, AF28, 0, (uint32_t)cfp, (uint32_t)(cfp >> 32), 0 };
      write_raw("ANN", "MODPARTS.DAT", hd, en, 1); }
    g_vp_have = 0;
    profile_load("ANN", 0xe000);
    convert(CUST, 0x434u);
    save("ANN", 0xe001);
    n = read_file("ANN", "MODPARTS.DAT", e, 1100);
    static uint32_t want[0xaa]; memcpy(want, own, sizeof want); want[0x40] = 0;
    CHECK(n == 1 && e[0].w[3] == AF28 && e[0].w[1] == ADDON && rec_is(&g_pay[2], ADDON, want), "kind0-unknown the car as it was (its own car type, the "
          "vinyl's stand-in): %d; ART_F_28 %s", rec_is(&g_pay[2], ADDON, want), n == 1 && e[0].w[3] == AF28 ? "kept for it" : "LOST");
    g_vp_have = 1; car_init(CUST, TUNED0, FOCUS, 1);

    /* names: a profile with a space in its name */
    car_init(CAR, SLOT1, PEUGOT, 0);
    profile_load("MY CAR", 0xf000);
    MR32(CAR + R_KEY(0x40u)) = AF28;
    save("MY CAR", 0xf001);
    n = read_file("MY CAR", "MODPARTS.DAT", e, 1100);
    CHECK(n == 1 && e[0].w[3] == AF28, "names \"MY CAR\": its MODPARTS.DAT %s", n == 1 ? "written" : "NOT written");
    MR32(CAR + R_KEY(0x40u)) = 0;

    /* foreign: a later page's MODPARTS.DAT kept aside, twice */
    clean("ANN");
    car_init(CAR, SLOT1, PEUGOT, 0);
    { uint32_t v2[4] = { 0x504d324eu, 2u, 1u, 0 }, a1[8] = { 0xAAAA, 0, 0, 0, 0, 0, 0, 9 }; write_raw("ANN", "MODPARTS.DAT", v2, a1, 1); }
    profile_load("ANN", 0x10000); MR32(CAR + R_KEY(0x40u)) = AF28; save("ANN", 0x10001);
    { uint32_t v2[4] = { 0x504d324eu, 2u, 1u, 0 }, b1[8] = { 0xBBBB, 0, 0, 0, 0, 0, 0, 9 }; write_raw("ANN", "MODPARTS.DAT", v2, b1, 1); }
    profile_load("ANN", 0x10002); save("ANN", 0x10003);
    uint32_t w1 = first_word("ANN", "MODPARTS.OLD"), w2 = first_word("ANN", "MODPARTS.OLD2");
    n = read_file("ANN", "MODPARTS.DAT", e, 1100);
    CHECK(w1 == 0xAAAA && w2 == 0xBBBB && n == 1, "foreign the first later page's file in MODPARTS.OLD: %s, the second in MODPARTS.OLD2: %s; ours "
          "written: %d", w1 == 0xAAAA ? "yes" : "NO", w2 == 0xBBBB ? "yes" : "NO", n == 1);
    MR32(CAR + R_KEY(0x40u)) = 0;

    /* deleted: the profile deleted in the game, a new one of its name */
    clean("ANN");
    car_init(CAR, SLOT1, PEUGOT, 0);
    { uint64_t fpc = car_fp(CAR); uint32_t hh[4] = { 0x504d324eu, 1u, 1u, 0 }, ee[8] = { SLOT1, PEUGOT, 0x40u, AF28, 0, (uint32_t)fpc, (uint32_t)(fpc >> 32), 0 };
      write_raw("ANN", "MODPARTS.DAT", hh, ee, 1); }
    g_vp_have = 0;
    profile_load("ANN", 0x11001);                                /* ART_F_28 waits */
    int waits = g_wait[0].n == 1;
    clean("ANN"); modparts_deleted("ANN");
    save("ANN", 0x11002);                                        /* the new profile ANN, saved without a load first */
    n = read_file("ANN", "MODPARTS.DAT", e, 1100);
    CHECK(waits && n == -1, "deleted before: a part waiting %d; the new profile of its name saved: %s", waits, n == -1 ? "nothing of the old one" : "the OLD ONE'S PARTS");
    g_vp_have = 1;

    /* cut-before-profile (M1): MODPARTS.NEW of a save written, the profile not (a page closed, a write failed): the
     * load of the profile on disk takes its own MODPARTS.DAT and removes that MODPARTS.NEW (the reviewer's R1) */
    clean("ANN");
    car_init(CAR, SLOT1, PEUGOT, 0); MR32(CAR + R_KEY(SLOT_RIM)) = BLITZ;
    profile_load("ANN", 0x12000);
    save("ANN", 0x12001);                                        /* MODPARTS.DAT: BLITZ; the profile 0x12001 */
    uint32_t keep10 = MR32(CAR + R_KEY(0x10u));
    MR32(CAR + R_KEY(0x10u)) = 0x51000999u;                      /* a part of the game's bought */
    save_as("ANN", 0x12002, 0, 0);                               /* MODPARTS.NEW staged; the profile never written */
    int staged = there("ANN", "MODPARTS.NEW");
    MR32(CAR + R_KEY(SLOT_RIM)) = AVUS; MR32(CAR + R_KEY(0x10u)) = keep10;   /* the profile on disk, 0x12001 */
    profile_load("ANN", 0x12001);
    int rim_back = MR32(CAR + R_KEY(SLOT_RIM)) == BLITZ, gone_new = !there("ANN", "MODPARTS.NEW");
    save("ANN", 0x12003);
    n = read_file("ANN", "MODPARTS.DAT", e, 1100);
    int blitz = 0; for (int i = 0; i < n; i++) blitz += e[i].w[3] == BLITZ;
    CHECK(staged && rim_back && gone_new && blitz == 1, "cut-before-profile the rim back from MODPARTS.DAT: %d, the unwritten save's MODPARTS.NEW "
          "removed: %d, the next save keeps the rim: %d", rim_back, gone_new, blitz);
    MR32(CAR + R_KEY(SLOT_RIM)) = stock_of(0, SLOT_RIM);

    /* new-profile (M2): a profile made this session (never loaded) counts its own writes; a cut rename after its
     * second save of parts loses nothing (the reviewer's R2) */
    { char dd[700]; snprintf(dd, sizeof dd, "%s/BOB", g_dir); mkdir(dd, 0755); }
    clean("BOB");
    car_init(CAR, SLOT1, PEUGOT, 0);
    save("BOB", 0x13001);
    MR32(CAR + R_KEY(SLOT_RIM)) = BLITZ;
    save("BOB", 0x13002);
    read_file("BOB", "MODPARTS.DAT", e, 1100); uint32_t b1 = g_hd3;
    MR32(CAR + R_KEY(0x40u)) = AF28;
    save_as("BOB", 0x13003, 0, 0);                               /* MODPARTS.NEW, the profile 0x13003, the rename cut */
    read_file("BOB", "MODPARTS.NEW", e, 1100); uint32_t b2 = g_hd3;
    MR32(CAR + R_KEY(SLOT_RIM)) = AVUS; MR32(CAR + R_KEY(0x40u)) = 0;
    profile_load("BOB", 0x13003);
    CHECK(b2 == b1 + 1u && MR32(CAR + R_KEY(0x40u)) == AF28 && MR32(CAR + R_KEY(SLOT_RIM)) == BLITZ, "new-profile its writes %u then %u; after the "
          "cut rename the vinyl %s, the rim %s", b1, b2, MR32(CAR + R_KEY(0x40u)) == AF28 ? "back" : "LOST", MR32(CAR + R_KEY(SLOT_RIM)) == BLITZ ? "back" : "LOST");
    MR32(CAR + R_KEY(SLOT_RIM)) = stock_of(0, SLOT_RIM); MR32(CAR + R_KEY(0x40u)) = 0;

    /* modded-keys (M3): a modded game's car whose own keys have a mod's rim and ART_F_28: written back with them, all
     * 170 keys as loaded, nothing of it beside the profile, and nothing to save while nothing changed (the reviewer's R3) */
    clean("ANN");
    car_init(CAR2, SLOT2, PEUGOT, 0); MR8(CAR2 + 0x7f5u) = 0; car_init(CUST, TUNED0, FOCUS, 1);
    { static uint32_t k3[0xaa]; for (uint32_t s = 0; s < 0xaau; s++) k3[s] = 0xadd00000u | s; k3[SLOT_RIM] = BLITZ; k3[0x40] = AF28;
      addon(CAR, SLOT1, ADDON, k3, 0x7f8u);
      profile_load("ANN", 0x14000); convert(CAR, 0x7f8u);
      kept_payload("ANN", 0x14001);
      int ch = modparts_changed(&g_c, 0), ch2 = modparts_changed(&g_c, 0);
      save("ANN", 0x14002);
      n = read_file("ANN", "MODPARTS.DAT", e, 1100);
      CHECK(!ch && !ch2 && rec_is(&g_pay[0], ADDON, k3) && n == -1, "modded-keys something to save with nothing changed: %d, %d; the payload's car "
            "%s; MODPARTS.DAT %s", ch, ch2, rec_is(&g_pay[0], ADDON, k3) ? "as loaded, the rim and the vinyl its own" : "NOT as loaded",
            n == -1 ? "none" : "MADE"); }
    car_init(CAR, SLOT1, PEUGOT, 0);

    char cmd[800]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir); if (system(cmd)) {}
    printf(g_fail ? "modparts_unit: FAILED\n" : "modparts_unit: passed\n");
    return g_fail;
}
