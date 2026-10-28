/* garage.c -- the career garage's 10 slots, and Trade Car: a car of yours
 * taken from the garage for 80 % of the money spent on it.
 *
 * The game's own garage (SPEED2.EXE, checked against players' saves):
 *  - the player's cars are records in FEDatabase +0x3C0 (CARS0, player 2's
 *    CARS_SZ on): 48 stock cars, 20 customized ones (Customize), the career's
 *    5 (+0x5954, 0x7F8 bytes each, their count 5 at +0x812C), 6 presets and
 *    12 others, and a table of the 91 (+0x9BD8, made by 0x5036F0) that every
 *    list walks.  A career car: +8 its handle (bStringHash of
 *    "CAREER_SLOT_<n>", 0x50B990), +0x14 4, +0x434 its parts (0x503E70 clears
 *    them), +0x7F0 the money spent on it (shop buys add theirs: 0x5289F0,
 *    0x547570), +0x7F4 locked, +0x7F5 a car in it;
 *  - a new career opens the first slot (0x532830) and the start of each
 *    stage the next one (0x541680 and the tail of 0x551320, 0x532CF0 ->
 *    0x5284D0: the first locked of the 5, once a stage: its reward mark
 *    +0x73C0 of the stage's 0x104 bytes at Career +0x72D8);
 *  - the car lot puts a car in the first open free slot (0x495FD0) or, all
 *    full, trades one of them in for it; cars cost nothing there and a trade
 *    pays nothing.  The language files have "Trade Car" (0xE0F68E78, the
 *    Online car view's) and "You really want to trade this in? You'll lose
 *    all the mods, and you'll only get 80% of its cost." (0xB18C1000, used by
 *    no code), all 13 languages: Trade Car here is that.
 *  - the profile saves the cars as they are (0x5166B0 / 0x516850, 2,034 bytes
 *    a career car at the payload's 22,596), a payload of 54,930 bytes in all.
 *
 * Here the 5 stay where they are, so whatever reads them as the game does
 * still finds 5 good ones, and 5 more (slots 6-10, CAREER_SLOT_5..9) are
 * records of the same class in guest memory of our own, one set for each of
 * the two players' cars (xbase).  What looks a career car up or walks the
 * list sees them after the game's own (the table's finds, 0x503510, 0x503680,
 * 0x5035C0; its walk 0x5162D0 and count 0x534850); the race's setups, which
 * walk the table themselves (0x525ED0, 0x526970, 0x53FEB0), find the one the
 * player drives in the table's last entry for as long as they run (swap_in);
 * the shop's buys (0x547570) find it by hook.  Slots open 2 a stage, 10 at
 * stage 5: each opening the game makes opens one more (opened).
 *
 * Slots 6-10 are saved beside the profile: "NFS Underground 2/<NAME>/GARAGE.DAT"
 * (the GARAGE file: GARAGE_MAGIC, a fingerprint of the profile's car part, the
 * cars driven, then the 5 records as the profile has its own), written when
 * the game has written its profile (fileio.c: garage_profile_written), from
 * the payload the game made for it (0x52A280, every time it makes one:
 * built; the save's through 0x52A450).  A profile whose GARAGE file is missing opens 2 slots a stage from
 * the ones it has (topup); one of another car part (the profile changed
 * elsewhere: the PC game) is kept aside as GARAGE.OLD, never written over.  The game's
 * "anything to save?" (0x52A4C0) compares a payload with the one it kept;
 * ours compares slots 6-10 too, and going back to the kept payload (0x5347C0)
 * puts them back as they were when it was made (kept), not as last saved.
 *
 * The game looks the car the career drives (Career +0x8740) and the Quick
 * Race car (FEDatabase +0xE8) up and uses them unchecked (0x527A1B ...), and
 * a profile load takes them as saved (0x529230, 0x5344D0).  So the profile
 * names a car of the 5 for one of slots 6-10 (stand), which the PC game can
 * load; the GARAGE file names the real ones, put back after the load as long
 * as the profile still names the stand-in.  A car the load leaves pointing at
 * an empty slot 6-10 becomes the first car you have.
 *
 * The page moves the file with its profile (renameProfile moves the folder),
 * backs it up and syncs it with the save folder. */
#include "cpu.h"
#include "widescreen.h"           /* HOOK_SITE */
#include <stdio.h>
#include <string.h>
#include <strings.h>

void dispatch_patch(uint32_t a, void (*f)(CPU *));
void dispatch_indirect(CPU *c, uint32_t addr);
uint32_t gheap_alloc(int heap, uint32_t n, int zero);
int fileio_resolve_save(const char *win, char *out, size_t outsz);

#define FEDB            0x0083a9d0u   /* FEDatabase */
#define FEDB_KEPT       0x16778u      /* ... the payload it compares with (0x52A460 makes it, 0x5032C0 frees it) */
#define CARS0           0x0083ad90u   /* player 1's cars (FEDatabase +0x3C0) */
#define CARS_SZ         0xab4cu       /* player 2's follow */
#define C_SLOTS         0x5954u       /* the career's 5 */
#define C_TABLE         0x9bd8u       /* the 91 records' table */
#define TABLE_N         91u
#define REC_SZ          0x7f8u
#define R_HANDLE        0x08u
#define R_USED          0x10u
#define R_FLAGS         0x14u         /* 4: a career car */
#define R_PARTS         0x434u        /* its parts, what it cost (0x503E70 clears them) */
#define R_SPENT         0x7f0u        /* the money spent on it */
#define R_LOCKED        0x7f4u
#define R_OWNED         0x7f5u
#define XN              5u            /* our slots: 6 to 10 */
#define SLOTS           10u
#define SAVE_REC        2034u         /* a career car in the profile */
#define CARS_PART       39230u        /* the payload's cars (0x5166B0's) */
#define P_NAME          53761u        /* the payload: the profile's name, 32 bytes */
#define PROFILE_SIZE    54966u
#define PAYLOAD_AT      36u
#define CAREER          0x0085ad40u
#define K_BANK          0x7134u
#define K_STAGES        0x73c0u       /* the first stage's reward mark (+0x72D8 +0xE8) */
#define K_STAGE_SZ      0x104u
#define K_STAGES_N      8u
#define K_STAGE         0x7af8u       /* the stage: 0 the prologue, 1-5, 6 the career done (0x529230 loads it) */
#define K_HAS_CAR       0x873du
#define K_CAR           0x8740u       /* the car the career drives (0x863480) */
#define G_GAMEMODE      0x00850078u   /* 1 career */
#define G_QR_CAR        0x0083aab8u   /* each player's car in Quick Race, LAN, Online (+4 player 2) */
#define G_HELPBAR       0x008384d8u   /* the PC help bar */
#define FN_REC_NEW      0x00525cf0u   /* thiscall record: its constructor (vtable 0x79B12C) */
#define FN_PARTS_CLEAR  0x00503e70u   /* thiscall record +0x434 */
#define FN_SLOT_HASH    0x0050b990u   /* cdecl (format, n) -> bStringHash of the text */
#define S_SLOT_FMT      0x00797e74u   /* "CAREER_SLOT_%d" */
#define FN_REC_WRITE    0x00516620u   /* thiscall record (out, &left) -> out after it: a career car as the profile has it */
#define FN_PARTS_READ   0x00504040u   /* thiscall record +0x434 (in, &left) -> in after them */
#define FN_SET_CAR      0x00500bd0u   /* thiscall Career (handle): the car it drives */
#define FN_DIALOG       0x00558790u   /* cdecl (package, "", 0xF770, 0x61C, yes, no, no, 0, text, %s, %s): two buttons */
#define FN_SHOW         0x00537c00u   /* cdecl (package, group, script name, 1) */
#define FN_SELECT_MAKE  0x004ee210u   /* thiscall UI_CareerCarSelect: its list and the car shown, again */
#define FN_HELPBAR_SET  0x0054e6e0u   /* thiscall help bar (buttons, package) */
#define FN_SELECT_MSG   0x004dfc50u   /* UI_CareerCarSelect's messages (its vtable's only) */
#define SELECT_PKG      "UI_CareerCarSelect.fng"
#define SC_PACKAGE      0x04u         /* a screen: its package's name */
#define SC_BUTTONS      0x20u         /* its help bar's buttons */
#define SC_ITEM         0x54u         /* UI_CareerCarSelect: the car shown (its list's) */
#define SC_AT           0x5cu         /* ... which of the list (1 the first: "1 of 10") */
#define IT_LOCKED       0x930u        /* ... a locked slot */
#define IT_FREE         0x931u        /* ... an open one with no car */
#define IT_HANDLE       0x920u
#define HB_PKG          0x58u         /* the help bar: the package its buttons are for */
#define HB_TRADE        0x40000u      /* the button Customize shows as Delete Tuned Car: PAD_BUTTON1 (C) */
#define TXT_TRADE_CAR   0xe0f68e78u   /* "Trade Car" */
#define TXT_TRADE_ASK   0xb18c1000u   /* "You really want to trade this in? ... 80% of its cost." */
#define MSG_BUTTON1     0xc519bfc0u
#define MSG_NO          0x34dc1bcfu   /* the screen's own: UNDIM */
#define MSG_TRADE_YES   0x6a7e1d05u   /* ours: Yes to TXT_TRADE_ASK */
#define S_GROUP         0x47ff4e7cu
#define S_UNDIM         0x0079031cu   /* "UNDIM" */
#define PR_CARS         0x3b0u        /* a player's profile (FEDatabase +0x10, 0x5344D0, 0x52A280): its cars */
#define GARAGE_FILE     "GARAGE.DAT"  /* with a dot: no profile's name (1-7 letters and digits), so not a profile's file */
#define GARAGE_OLD      "GARAGE.OLD"
#define GARAGE_MAGIC    0x4147324eu   /* "N2GA" */
#define TRADE_PCT       80u

static uint32_t g_blk;            /* guest memory: our records (XN a player), a new one, scratch */
#define X_AT(p)         (g_blk + (p) * XN * REC_SZ)
#define TEMPLATE        (g_blk + 2u * XN * REC_SZ)
#define SCRATCH         (TEMPLATE + REC_SZ)          /* XN * SAVE_REC, then the "left" counter */
#define SCRATCH_LEFT    (SCRATCH + XN * SAVE_REC)
#define BLK_SZ          (SCRATCH_LEFT + 16u - g_blk)

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
    char b[64]; uint32_t i = 0;
    if (!s || !mem_is_committed(s)) return 0;
    for (; i + 1 < sizeof b && mem_is_committed(s + i) && MR8(s + i); i++) b[i] = (char)MR8(s + i);
    b[i] = 0;
    return !strcasecmp(b, name);
}
static uint64_t fnv(uint32_t a, uint32_t n){
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < n; i++){ h ^= MR8(a + i); h *= 0x100000001b3ull; }
    return h;
}

/* --- the slots ------------------------------------------------------------- */
/* which player's cars db is (0, 1), -1: none of theirs */
static int player_of(uint32_t db){ return db == CARS0 ? 0 : db == CARS0 + CARS_SZ ? 1 : -1; }
static uint32_t slot(uint32_t db, unsigned i){
    int p = player_of(db);
    return i < 5u ? db + C_SLOTS + i * REC_SZ : X_AT(p) + (i - 5u) * REC_SZ;
}
/* one of our records, the handle h's, of db's: 0 none */
static uint32_t xfind(uint32_t db, uint32_t h){
    int p = player_of(db);
    if (p < 0 || !g_blk || !h) return 0;
    for (unsigned i = 0; i < XN; i++){ uint32_t r = X_AT(p) + i * REC_SZ; if (MR32(r + R_HANDLE) == h) return r; }
    return 0;
}
static int xis(uint32_t db, uint32_t r){
    int p = player_of(db);
    return p >= 0 && g_blk && r >= X_AT(p) && r < X_AT(p) + XN * REC_SZ;
}
static unsigned count(uint32_t db, uint32_t field, int set){
    unsigned n = 0;
    for (unsigned i = 0; i < SLOTS; i++) n += (MR8(slot(db, i) + field) != 0) == set;
    return n;
}
/* which slot the record r is (0: slot 1), -1: none */
static int slot_of(uint32_t db, uint32_t r){
    for (unsigned i = 0; i < SLOTS; i++) if (slot(db, i) == r) return (int)i;
    return -1;
}
#define open_n(db)      count(db, R_LOCKED, 0)
#define owned_n(db)     count(db, R_OWNED, 1)
/* the first locked slot open; 0: none */
static int open_next(uint32_t db){
    for (unsigned i = 0; i < SLOTS; i++) if (MR8(slot(db, i) + R_LOCKED)){ MR8(slot(db, i) + R_LOCKED) = 0; return 1; }
    return 0;
}
static void open_to(uint32_t db, unsigned n){ while (open_n(db) < (n > SLOTS ? SLOTS : n) && open_next(db)) {} }

/* a record as a new one, as the game's 0x516480 makes a slot: no car, locked */
static void fresh(uint32_t r, uint32_t handle, int locked){
    for (uint32_t o = 0; o < REC_SZ; o += 4u) MR32(r + o) = MR32(TEMPLATE + o);
    MR32(r + R_HANDLE) = handle; MR32(r + R_USED) = 1u; MR32(r + R_FLAGS) = 4u;
    MR8(r + R_LOCKED) = (uint8_t)locked; MR8(r + R_OWNED) = 0;
}
static void xinit(CPU *c, uint32_t db){
    int p = player_of(db);
    for (unsigned i = 0; i < XN; i++){
        uint32_t a[2] = { S_SLOT_FMT, 5u + i };
        fresh(X_AT(p) + i * REC_SZ, gcall(c, FN_SLOT_HASH, 0, 2, a), 1);
    }
}
/* our memory and records, the first time anything asks */
static int ensure(CPU *c){
    if (g_blk) return 1;
    uint32_t b = gheap_alloc(1, 2u * XN * REC_SZ + REC_SZ + XN * SAVE_REC + 16u, 1);
    if (!b){ fprintf(stderr, "[garage] no guest memory for slots 6-10\n"); return 0; }
    g_blk = b;
    gcall(c, FN_REC_NEW, TEMPLATE, 0, 0);
    gcall(c, FN_PARTS_CLEAR, TEMPLATE + R_PARTS, 0, 0);
    xinit(c, CARS0); xinit(c, CARS0 + CARS_SZ);
    return 1;
}

/* our 5 as the profile has its own 5 (2,034 bytes each), into out */
static void xsave(CPU *c, uint32_t db, uint8_t *out){
    int p = player_of(db);
    MR32(SCRATCH_LEFT) = XN * SAVE_REC;
    for (unsigned i = 0; i < XN; i++){ uint32_t a[2] = { SCRATCH + i * SAVE_REC, SCRATCH_LEFT }; gcall(c, FN_REC_WRITE, X_AT(p) + i * REC_SZ, 2, a); }
    for (uint32_t i = 0; i < XN * SAVE_REC; i++) out[i] = MR8(SCRATCH + i);
}
/* ... and back, as 0x516850 reads the profile's: 20 bytes from +4, 0x418 from
 * +0x18, +0x430, the two flags, the parts */
static void xload(CPU *c, uint32_t db, const uint8_t *in){
    int p = player_of(db);
    for (uint32_t i = 0; i < XN * SAVE_REC; i++) MR8(SCRATCH + i) = in[i];
    MR32(SCRATCH_LEFT) = XN * SAVE_REC;
    for (unsigned i = 0; i < XN; i++){
        uint32_t r = X_AT(p) + i * REC_SZ, s = SCRATCH + i * SAVE_REC;
        for (uint32_t o = 0; o < 20u; o += 4u) MR32(r + 4u + o) = MR32(s + o);
        for (uint32_t o = 0; o < 0x418u; o += 4u) MR32(r + 0x18u + o) = MR32(s + 20u + o);
        MR32(r + 0x430u) = MR32(s + 20u + 0x418u);
        MR8(r + R_LOCKED) = MR8(s + 1072u); MR8(r + R_OWNED) = MR8(s + 1073u);
        uint32_t a[2] = { s + 1074u, SCRATCH_LEFT };
        gcall(c, FN_PARTS_READ, r + R_PARTS, 2, a);
    }
}

/* --- the lists ------------------------------------------------------------- */
/* thiscall cars (handle) -> the record, 0: none.  Finds the table's, then ours. */
void sub_00503510_orig(CPU *c);
void sub_00503510(CPU *c){
    uint32_t db = c->ecx, h = MR32(c->esp + 4u);
    sub_00503510_orig(c);
    if (!c->eax) c->eax = xfind(db, h);
}
/* thiscall cars (handle) -> a career car, 0: none */
void sub_00503680_orig(CPU *c);
void sub_00503680(CPU *c){
    uint32_t db = c->ecx, h = MR32(c->esp + 4u);
    sub_00503680_orig(c);
    if (!c->eax) c->eax = xfind(db, h);
}
/* thiscall cars (handle) -> a customized or career car, 0: none */
void sub_005035c0_orig(CPU *c);
void sub_005035c0(CPU *c){
    uint32_t db = c->ecx, h = MR32(c->esp + 4u);
    sub_005035c0_orig(c);
    if (!c->eax) c->eax = xfind(db, h);
}
/* thiscall cars (kinds, after) -> the next record in use of those kinds, 0: no more */
static uint32_t xnext(uint32_t db, uint32_t kinds, uint32_t after){
    int p = player_of(db);
    unsigned i = 0;
    if (after) i = (after - X_AT(p)) / REC_SZ + 1u;
    for (; i < XN; i++){ uint32_t r = X_AT(p) + i * REC_SZ; if (MR32(r + R_USED) && (MR32(r + R_FLAGS) & kinds)) return r; }
    return 0;
}
void sub_005162d0_orig(CPU *c);
void sub_005162d0(CPU *c){
    uint32_t db = c->ecx, kinds = MR32(c->esp + 4u), after = MR32(c->esp + 8u);
    if (player_of(db) < 0 || !g_blk){ sub_005162d0_orig(c); return; }
    if (xis(db, after)){ POP32(c->eip); c->esp += 8u; c->eax = xnext(db, kinds, after); return; }
    sub_005162d0_orig(c);
    if (!c->eax) c->eax = xnext(db, kinds, 0);
}
/* thiscall cars (kinds, what) -> how many in use of those kinds; a career
 * car counts with a car in it */
void sub_00534850_orig(CPU *c);
void sub_00534850(CPU *c){
    uint32_t db = c->ecx, kinds = MR32(c->esp + 4u);
    int p = player_of(db);
    sub_00534850_orig(c);
    if (p < 0 || !g_blk || !(kinds & 4u)) return;
    uint32_t n = c->eax;
    for (unsigned i = 0; i < XN; i++){
        uint32_t r = X_AT(p) + i * REC_SZ;
        if (!MR32(r + R_USED) || !(MR32(r + R_FLAGS) & kinds) || !MR8(r + R_OWNED)) continue;
        if (gcall(c, MR32(MR32(r) + 4u), r, 0, 0) != 0xffffffffu) n++;      /* its car type: one of the game's */
    }
    c->eax = n;
}

/* The race's setups look each player's car up in their table themselves:
 * one of ours goes in the table's last entry (a preset) while they run. */
static uint32_t g_sw_at[2], g_sw_was[2];
static void swap_in(void){
    int career = MR32(G_GAMEMODE) == 1u;
    for (int p = 0; p < 2; p++){
        uint32_t db = CARS0 + (uint32_t)p * CARS_SZ;
        uint32_t h = career ? MR32(CAREER + K_CAR) : MR32(G_QR_CAR + 4u * (uint32_t)p);
        uint32_t x = career && p ? 0 : xfind(db, h);
        g_sw_at[p] = 0;
        if (!x) continue;
        g_sw_at[p] = db + C_TABLE + 4u * (TABLE_N - 1u);
        g_sw_was[p] = MR32(g_sw_at[p]);
        MR32(g_sw_at[p]) = x;
        fprintf(stderr, "[garage] the race's cars: player %d's from slot %u\n", p + 1, 6u + (x - X_AT(p)) / REC_SZ);
    }
}
static void swap_out(void){
    for (int p = 0; p < 2; p++) if (g_sw_at[p]){ MR32(g_sw_at[p]) = g_sw_was[p]; g_sw_at[p] = 0; }
}
void sub_00525ed0_orig(CPU *c);
void sub_00525ed0(CPU *c){ swap_in(); sub_00525ed0_orig(c); swap_out(); }
void sub_00526970_orig(CPU *c);
void sub_00526970(CPU *c){ swap_in(); sub_00526970_orig(c); swap_out(); }
void sub_0053feb0_orig(CPU *c);
void sub_0053feb0(CPU *c){ swap_in(); sub_0053feb0_orig(c); swap_out(); }

/* the runtime's own lookups (discord.c, mprace.c): the table's, then ours */
uint32_t garage_find(uint32_t db, uint32_t h){
    if (!h) return 0;
    for (uint32_t i = 0; i < TABLE_N; i++){ uint32_t r = MR32(db + C_TABLE + 4u * i); if (r && MR32(r + R_HANDLE) == h) return r; }
    return xfind(db, h);
}

/* --- new careers, stages --------------------------------------------------- */
/* thiscall cars: the 5 slots as a new profile's; ours too */
void sub_0052a5d0_orig(CPU *c);
void sub_0052a5d0(CPU *c){ sub_0052a5d0_orig(c); ensure(c); }
void sub_00516480_orig(CPU *c);
void sub_00516480(CPU *c){
    uint32_t db = c->ecx;
    sub_00516480_orig(c);
    if (player_of(db) >= 0 && ensure(c)) xinit(c, db);
}
/* thiscall Career (1): a new career, its first slot open; and the second */
void sub_00532830_orig(CPU *c);
void sub_00532830(CPU *c){
    sub_00532830_orig(c);
    if (g_blk) open_to(CARS0, 2u);
}
/* Each stage's reward opens a slot (the game's), and one more: compared on
 * the stages' reward marks.  Career::Update (0x551320) gives them, from its
 * tail 0x5417F0 and 0x541680; 0x532CF0 too (from 0x4C1340).  Not a
 * profile's marks loaded meanwhile (g_loads). */
static int g_in_stage;
static unsigned g_loads;
static void stages(CPU *c, void (*orig)(CPU *)){
    if (g_in_stage || !g_blk){ orig(c); return; }
    uint32_t was[K_STAGES_N]; unsigned n = 0, open0 = open_n(CARS0), loads = g_loads;
    for (unsigned k = 0; k < K_STAGES_N; k++) was[k] = MR32(CAREER + K_STAGES + k * K_STAGE_SZ);
    g_in_stage = 1; orig(c); g_in_stage = 0;
    for (unsigned k = 0; k < K_STAGES_N; k++) n += !was[k] && MR32(CAREER + K_STAGES + k * K_STAGE_SZ);
    if (!n || loads != g_loads) return;
    open_to(CARS0, open0 + 2u * n);
    fprintf(stderr, "[garage] a new stage: %u of %u slots open\n", open_n(CARS0), SLOTS);
}
void sub_00551320_orig(CPU *c);
void sub_00551320(CPU *c){ stages(c, sub_00551320_orig); }
void sub_00532cf0_orig(CPU *c);
void sub_00532cf0(CPU *c){ stages(c, sub_00532cf0_orig); }
/* thiscall FEDatabase: Online's end puts player 1's cars back as its copy has
 * them (+0x1677C, as made); ours as made too */
void sub_00515d70_orig(CPU *c);
void sub_00515d70(CPU *c){ sub_00515d70_orig(c); if (g_blk) xinit(c, CARS0); }

/* --- the profile ------------------------------------------------------------ */
/* What a profile's payload leaves to us: slots 6-10, and the cars driven when
 * they are among them (player 1's: the career's and the Quick Race car), with
 * the car of the 5 the payload names instead. */
typedef struct { uint32_t drive, qr, stand; uint8_t cars[XN * SAVE_REC]; } State;
static struct { char name[33]; uint64_t fp; State st; unsigned seq; } g_built[4];
static unsigned g_seq;
static struct { int on, p; char name[33]; uint64_t fp; State st; } g_kept;   /* as the kept payload was made */
static State g_now, g_read;
static struct { int on; uint32_t drive, qr, stand; } g_back;     /* for 0x5344D0's end: the cars driven */
static int g_topup;                                               /* ... and 2 slots a stage */
static char g_topup_name[33];

static void name_of(uint32_t payload, char *out){
    unsigned i = 0;
    for (; i < 32u && MR8(payload + P_NAME + i); i++) out[i] = (char)MR8(payload + P_NAME + i);
    out[i] = 0;
}
/* the record of ours with the handle h with a car in it, 0: none */
static uint32_t xowned(uint32_t db, uint32_t h){ uint32_t r = xfind(db, h); return r && MR8(r + R_OWNED) ? r : 0; }
/* the handle of the first of the first n slots with a car in it, 0: none */
static uint32_t first_car(uint32_t db, unsigned n){
    for (unsigned i = 0; i < n; i++){ uint32_t r = slot(db, i); if (MR8(r + R_OWNED)) return MR32(r + R_HANDLE); }
    return 0;
}
static void state_of(CPU *c, uint32_t db, State *st){
    xsave(c, db, st->cars);
    st->drive = st->qr = st->stand = 0;
    if (db != CARS0) return;
    uint32_t stand = first_car(CARS0, 5u);
    if (!stand) return;
    if (xowned(CARS0, MR32(CAREER + K_CAR))) st->drive = MR32(CAREER + K_CAR);
    if (xowned(CARS0, MR32(G_QR_CAR))) st->qr = MR32(G_QR_CAR);
    if (st->drive || st->qr) st->stand = stand;
}
static int differ(const State *a, const State *b){
    return a->drive != b->drive || a->qr != b->qr || memcmp(a->cars, b->cars, sizeof a->cars);
}
/* thiscall profile (out, size): the payload, naming the stand-in for a car of
 * ours driven; what our slots are in it */
void sub_0052a280_orig(CPU *c);
void sub_0052a280(CPU *c){
    uint32_t db = c->ecx + PR_CARS, out = MR32(c->esp + 4u), size = MR32(c->esp + 8u);
    int p = player_of(db);
    if (!g_blk || p < 0 || size < P_NAME + 32u){ sub_0052a280_orig(c); return; }
    static State st0;
    State *st = &st0;
    state_of(c, db, st);
    uint32_t k = MR32(CAREER + K_CAR), q = MR32(G_QR_CAR);
    if (st->drive) MR32(CAREER + K_CAR) = st->stand;
    if (st->qr) MR32(G_QR_CAR) = st->stand;
    sub_0052a280_orig(c);
    MR32(CAREER + K_CAR) = k; MR32(G_QR_CAR) = q;
    /* kept by name and car part: the newest of each (a payload made again
     * while nothing changed takes its place, not an older one's) */
    char name[33]; name_of(out, name);
    uint64_t fp = fnv(out, CARS_PART);
    unsigned at = 0;
    for (unsigned i = 1; i < 4u; i++) if (g_built[i].seq < g_built[at].seq) at = i;
    for (unsigned i = 0; i < 4u; i++) if (g_built[i].seq && g_built[i].fp == fp && !strcmp(g_built[i].name, name)){ at = i; break; }
    memcpy(g_built[at].name, name, sizeof name);
    g_built[at].fp = fp;
    g_built[at].st = *st;
    g_built[at].seq = ++g_seq;
    st = &g_built[at].st;
    if (out == MR32(FEDB + FEDB_KEPT)){
        g_kept.st = *st; g_kept.p = p; g_kept.fp = g_built[at].fp; g_kept.on = 1;
        memcpy(g_kept.name, g_built[at].name, sizeof g_kept.name);
    }
}
/* thiscall FEDatabase (out, size): the payload the save writes (the memory
 * card's 0x4D9B80): add ecx, 0x10; jmp 0x52A280.  The lift copies a jump's
 * target into the function that jumps, so its own copy of 0x52A280 would
 * not be ours. */
void sub_0052a450_orig(CPU *c);
void sub_0052a450(CPU *c){ c->ecx += 0x10u; sub_0052a280(c); }
/* thiscall FEDatabase -> al: anything to save (its payload now against the one it kept) */
void sub_0052a4c0_orig(CPU *c);
void sub_0052a4c0(CPU *c){
    uint32_t kept = MR32(c->ecx + FEDB_KEPT);
    sub_0052a4c0_orig(c);
    if ((c->eax & 0xffu) || !kept || !g_kept.on || !g_blk) return;
    state_of(c, CARS0 + (uint32_t)g_kept.p * CARS_SZ, &g_now);
    if (differ(&g_now, &g_kept.st)){ c->eax = (c->eax & ~0xffu) | 1u; fprintf(stderr, "[garage] slots 6-10 changed: something to save\n"); }
}

/* fileio.c: the game has written a profile (host path): its GARAGE file beside it */
void garage_profile_written(const char *path){
    static uint8_t f[PROFILE_SIZE];
    FILE *in = fopen(path, "rb");
    if (!in) return;
    size_t n = fread(f, 1, sizeof f, in); fclose(in);
    if (n != PROFILE_SIZE || memcmp(f, "20CM", 4)) return;
    const uint8_t *pl = f + PAYLOAD_AT;
    char name[33]; unsigned i = 0;
    for (; i < 32u && pl[P_NAME + i]; i++) name[i] = (char)pl[P_NAME + i];
    name[i] = 0;
    uint64_t fp = 0xcbf29ce484222325ull;
    for (uint32_t k = 0; k < CARS_PART; k++){ fp ^= pl[k]; fp *= 0x100000001b3ull; }
    int best = -1;
    for (int k = 0; k < 4; k++)
        if (g_built[k].seq && g_built[k].fp == fp && !strcasecmp(g_built[k].name, name) && (best < 0 || g_built[k].seq > g_built[best].seq)) best = k;
    if (best < 0){ fprintf(stderr, "[garage] %s written: no payload of ours for it, its GARAGE file left as it is\n", name); return; }
    const State *st = &g_built[best].st;
    char g[1100]; const char *sl = strrchr(path, '/');
    snprintf(g, sizeof g, "%.*s/%s", sl ? (int)(sl - path) : 1, sl ? path : ".", GARAGE_FILE);
    FILE *o = fopen(g, "wb");
    if (!o){ fprintf(stderr, "[garage] %s: cannot write %s\n", name, g); return; }
    uint32_t hd[8] = { GARAGE_MAGIC, 1u, (uint32_t)fp, (uint32_t)(fp >> 32), st->drive, st->qr, st->stand, 0 };
    int ok = fwrite(hd, sizeof hd, 1, o) == 1 && fwrite(st->cars, sizeof st->cars, 1, o) == 1;
    ok = fclose(o) == 0 && ok;
    unsigned cars = 0;
    for (unsigned k = 0; k < XN; k++) cars += st->cars[k * SAVE_REC + 1073u] != 0;
    fprintf(stderr, "[garage] %s: slots 6-10 saved (%u cars)%s%s\n", name, cars,
            st->drive ? ", the career's car one of them" : "", ok ? "" : ": WRITE FAILED");
}
/* fileio.c: a profile file deleted: its GARAGE files go too (the game then removes the folder) */
void garage_profile_deleted(const char *path){
    static const char *const files[] = { GARAGE_FILE, GARAGE_OLD };
    char g[1100]; const char *sl = strrchr(path, '/');
    for (unsigned i = 0; i < 2u; i++){
        snprintf(g, sizeof g, "%.*s/%s", sl ? (int)(sl - path) : 1, sl ? path : ".", files[i]);
        if (remove(g) == 0) fprintf(stderr, "[garage] %s deleted with its profile\n", g);
    }
}

/* slots 6-10 of the profile named name whose car part has fingerprint fp, 1:
 * found.  A GARAGE file of another car part is kept aside as GARAGE.OLD: the
 * next save would write over it. */
static int read_garage(const char *name, uint64_t fp, State *st){
    char win[128], path[1100];
    snprintf(win, sizeof win, "NFS Underground 2\\%s\\%s", name, GARAGE_FILE);
    if (!fileio_resolve_save(win, path, sizeof path)) return 0;
    FILE *in = fopen(path, "rb");
    if (!in) return 0;
    uint32_t hd[8] = { 0 };
    int ok = fread(hd, sizeof hd, 1, in) == 1 && fread(st->cars, sizeof st->cars, 1, in) == 1;
    fclose(in);
    if (!ok || hd[0] != GARAGE_MAGIC || hd[1] != 1u){ fprintf(stderr, "[garage] %s: its GARAGE file is not one of ours\n", name); return 0; }
    if (hd[2] != (uint32_t)fp || hd[3] != (uint32_t)(fp >> 32)){
        char old[1100]; const char *sl = strrchr(path, '/');
        snprintf(old, sizeof old, "%.*s/%s", sl ? (int)(sl - path) : 1, sl ? path : ".", GARAGE_OLD);
        int kept = rename(path, old) == 0;
        fprintf(stderr, "[garage] %s: its GARAGE file is of another save of it (the profile changed elsewhere): not used, %s\n",
                name, kept ? "kept as " GARAGE_OLD : "COULD NOT KEEP IT ASIDE");
        return 0;
    }
    st->drive = hd[4]; st->qr = hd[5]; st->stand = hd[6];
    return 1;
}
static void load_state(CPU *c, uint32_t db, const State *st){
    xload(c, db, st->cars);
    if (db == CARS0 && (st->drive || st->qr)){ g_back.on = 1; g_back.drive = st->drive; g_back.qr = st->qr; g_back.stand = st->stand; }
}
/* thiscall cars (in, size): a profile's cars into these; ours from its GARAGE
 * file, or, the game going back to the payload it kept (0x5347C0), as they
 * were when it was made */
void sub_00516850_orig(CPU *c);
void sub_00516850(CPU *c){
    uint32_t db = c->ecx, in = MR32(c->esp + 4u), size = MR32(c->esp + 8u);
    sub_00516850_orig(c);
    g_loads++;
    int p = player_of(db);
    if (p < 0 || !ensure(c)) return;
    if (!p) g_back.on = 0;
    xinit(c, db);
    char name[33] = "";
    uint64_t fp = fnv(in, CARS_PART);
    if (size >= P_NAME + 32u) name_of(in, name);
    if (g_kept.on && g_kept.p == p && in == MR32(FEDB + FEDB_KEPT) && fp == g_kept.fp && (!name[0] || !strcmp(name, g_kept.name))){
        load_state(c, db, &g_kept.st);
        fprintf(stderr, "[garage] back to the kept payload: slots 6-10 as then, %u cars of yours, %u of %u slots open\n",
                owned_n(db), open_n(db), SLOTS);
        return;
    }
    if (name[0]){
        if (read_garage(name, fp, &g_read)){
            load_state(c, db, &g_read);
            fprintf(stderr, "[garage] %s: slots 6-10 from its GARAGE file, %u cars of yours, %u of %u slots open\n",
                    name, owned_n(db), open_n(db), SLOTS);
            return;
        }
        /* 2 a stage, once the career (its stage) has loaded too: 0x5344D0's end */
        if (!p){ g_topup = 1; memcpy(g_topup_name, name, sizeof g_topup_name); }
        return;
    }
    /* the cars alone (0x534770): the game's own rule had a slot a stage */
    open_to(db, 2u * open_n(db));
    fprintf(stderr, "[garage] (cars only): no slots 6-10 saved, %u of %u slots open\n", open_n(db), SLOTS);
}
/* thiscall FEDatabase (in, size): a profile loaded (0x4A9850), in
 * [+0x156A8] 4 only the cars of player [+0x20358], by a jump to 0x516850 (a
 * copy of it in the lift: see 0x52A450); else 0x5344D0, a call */
void sub_00534770_orig(CPU *c);
void sub_00534770(CPU *c){
    uint32_t fe = c->ecx;
    if (MR32(fe + 0x156a8u) != 4u){ sub_00534770_orig(c); return; }
    c->ecx = fe + 0x10u + MR32(fe + 0x20358u) * CARS_SZ + PR_CARS;
    MR32(c->esp + 8u) = CARS_PART;
    sub_00516850(c);                             /* its ret 8 is this one's */
}
/* a car handle at at (the career's, the Quick Race one) that names an empty
 * slot 6-10: the first car you have */
static void not_empty(uint32_t at, const char *what){
    uint32_t h = MR32(at), r = xfind(CARS0, h), to;
    if (!r || MR8(r + R_OWNED) || !(to = first_car(CARS0, SLOTS))) return;
    MR32(at) = to;
    fprintf(stderr, "[garage] %s was in the empty slot %d: now the one in slot %d\n", what, slot_of(CARS0, r) + 1, slot_of(CARS0, garage_find(CARS0, to)) + 1);
}
/* thiscall profile (in, size, 1): a profile loaded (its cars 0x516850, the
 * career 0x529230, the options); the cars driven as they were, if the
 * profile still names the stand-in */
void sub_005344d0_orig(CPU *c);
void sub_005344d0(CPU *c){
    uint32_t db = c->ecx + PR_CARS;
    g_topup = 0;
    sub_005344d0_orig(c);
    if (!g_blk || db != CARS0) return;
    if (g_topup){
        /* no slots 6-10 saved (an older page's profile, the PC game's): 2 a
         * stage, as the slots the profile has open may be the game's 1 a
         * stage or more (a profile of ours without its GARAGE file) */
        uint32_t stage = MR32(CAREER + K_STAGE);
        open_to(CARS0, stage <= 1u ? 2u : 2u * stage);
        fprintf(stderr, "[garage] %s: no slots 6-10 saved, stage %u: %u of %u slots open\n", g_topup_name, stage, open_n(CARS0), SLOTS);
        g_topup = 0;
    }
    if (g_back.on){
        uint32_t r;
        if (g_back.drive && MR32(CAREER + K_CAR) == g_back.stand && (r = xowned(CARS0, g_back.drive))){
            MR32(CAREER + K_CAR) = g_back.drive;
            fprintf(stderr, "[garage] the career drives the car in slot %d\n", slot_of(CARS0, r) + 1);
        }
        if (g_back.qr && MR32(G_QR_CAR) == g_back.stand && xowned(CARS0, g_back.qr)) MR32(G_QR_CAR) = g_back.qr;
        g_back.on = 0;
    }
    not_empty(CAREER + K_CAR, "the career's car");
    not_empty(G_QR_CAR, "the Quick Race car");
}

/* The shop's buys (0x547570) add each part to the career car it finds among
 * the 5 by handle; none: esi 0, and the part went to address 0x434. */
int garage_hook(CPU *c, uint32_t ip){
    if (ip != HOOK_SITE(0x00547685)) return 0;    /* mov ecx, [esp+0x10]: esi the car or 0 */
    if (!c->esi){
        uint32_t x = xfind(CARS0, MR32(CAREER + K_CAR));
        if (x){ c->esi = x; fprintf(stderr, "[garage] the shop's buy: for the car in slot %u\n", 6u + (x - X_AT(0)) / REC_SZ); }
    }
    return 1;
}

/* --- Trade Car -------------------------------------------------------------- */
/* thiscall help bar (buttons, package): the garage's car select has Trade Car with 2 cars or more */
void sub_0054e6e0_orig(CPU *c);
void sub_0054e6e0(CPU *c){
    if (g_blk && MR32(G_GAMEMODE) == 1u && pkg_is(MR32(c->esp + 8u), SELECT_PKG)){
        unsigned cars = owned_n(CARS0);
        if (cars >= 2u) MR32(c->esp + 4u) |= HB_TRADE;
        fprintf(stderr, "[garage] the garage: %u cars, %u of %u slots open%s\n", cars, open_n(CARS0), SLOTS, cars >= 2u ? ", Trade Car" : "");
    }
    sub_0054e6e0_orig(c);
}
/* thiscall help bar (button) -> its label's text */
void sub_0052d330_orig(CPU *c);
void sub_0052d330(CPU *c){
    uint32_t hb = c->ecx, b = MR32(c->esp + 4u);
    sub_0052d330_orig(c);
    if (b == HB_TRADE && pkg_is(MR32(hb + HB_PKG), SELECT_PKG)) c->eax = TXT_TRADE_CAR;
}

static uint32_t g_trade;          /* the handle Trade Car asks about */
static void handles_move(uint32_t from, uint32_t to){
    if (MR32(CAREER + K_CAR) == from) MR32(CAREER + K_CAR) = to;
    if (MR32(G_QR_CAR) == from) MR32(G_QR_CAR) = to;
}
static int trade_ask(CPU *c, uint32_t scr){
    uint32_t it = MR32(scr + SC_ITEM);
    if (!g_blk || !it || MR8(it + IT_LOCKED) || MR8(it + IT_FREE) || owned_n(CARS0) < 2u) return 0;
    uint32_t h = MR32(it + IT_HANDLE), r = garage_find(CARS0, h);
    if (!r || slot_of(CARS0, r) < 0 || !MR8(r + R_OWNED)) return 0;
    g_trade = h;
    uint32_t a[11] = { MR32(scr + SC_PACKAGE), 0x00783fe5u, 0xf770u, 0x61cu, MSG_TRADE_YES, MSG_NO, MSG_NO, 0, TXT_TRADE_ASK, 0, 0 };
    gcall(c, FN_DIALOG, 0, 11, a);
    return 1;
}
static void trade(CPU *c, uint32_t scr){
    uint32_t r = garage_find(CARS0, g_trade);
    int s = r ? slot_of(CARS0, r) : -1;
    uint32_t pkg = MR32(scr + SC_PACKAGE);
    uint32_t a[4] = { pkg, S_GROUP, S_UNDIM, 1u };
    gcall(c, FN_SHOW, 0, 4, a);
    if (s < 0 || !MR8(r + R_OWNED) || owned_n(CARS0) < 2u) return;
    uint32_t spent = MR32(r + R_SPENT), pay = (uint32_t)((uint64_t)spent * TRADE_PCT / 100u);
    MR32(CAREER + K_BANK) += pay;
    fresh(r, g_trade, 0);
    /* the game counts on slot 1 having a car (the car lot's Back): the last car moves there */
    if (s == 0){
        for (int k = (int)SLOTS - 1; k > 0; k--){
            uint32_t q = slot(CARS0, (unsigned)k);
            if (!MR8(q + R_OWNED)) continue;
            uint32_t hq = MR32(q + R_HANDLE);
            for (uint32_t o = 4u; o < REC_SZ; o += 4u) if (o != R_HANDLE) MR32(r + o) = MR32(q + o);
            fresh(q, hq, 0);
            handles_move(hq, g_trade);
            fprintf(stderr, "[garage] the car in slot %d moved to slot 1\n", k + 1);
            break;
        }
    }
    /* the one you drive, and the Quick Race car: not the empty slot, the first car you have */
    uint32_t kr = garage_find(CARS0, MR32(CAREER + K_CAR)), qr = garage_find(CARS0, MR32(G_QR_CAR));
    if (kr && slot_of(CARS0, kr) >= 0 && !MR8(kr + R_OWNED)){
        uint32_t h = first_car(CARS0, SLOTS);
        gcall(c, FN_SET_CAR, CAREER, 1, &h);
    }
    if (qr && slot_of(CARS0, qr) >= 0 && !MR8(qr + R_OWNED)) MR32(G_QR_CAR) = first_car(CARS0, SLOTS);
    fprintf(stderr, "[garage] traded the car in slot %d for $%u (%u %% of $%u spent on it), %u cars left, the bank $%u\n",
            s + 1, pay, TRADE_PCT, spent, owned_n(CARS0), MR32(CAREER + K_BANK));
    MR32(scr + SC_AT) = 1u;                      /* the list starts again at its first */
    gcall(c, FN_SELECT_MAKE, scr, 0, 0);
    uint32_t hb[2] = { MR32(scr + SC_BUTTONS), pkg };
    gcall(c, FN_HELPBAR_SET, MR32(G_HELPBAR), 2, hb);
}
/* UI_CareerCarSelect's messages (msg, a, b, c; ret 0x10): C or the help bar's Trade Car asks, Yes trades */
void sub_004dfc50(CPU *c);
static void select_msg(CPU *c){
    uint32_t m = MR32(c->esp + 4u), scr = c->ecx;
    if (m == MSG_TRADE_YES || (m == MSG_BUTTON1 && trade_ask(c, scr))){
        if (m == MSG_TRADE_YES) trade(c, scr);
        POP32(c->eip); c->esp += 0x10u;
        return;
    }
    sub_004dfc50(c);
}

void garage_install(void){
    dispatch_patch(FN_SELECT_MSG, select_msg);
}
