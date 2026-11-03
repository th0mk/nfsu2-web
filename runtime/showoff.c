/* showoff.c -- free roam's show-off: with the car standing, O (or the
 * controller's D-pad down held for a second) opens its parts a step at a
 * time: the trunk, then the hood too, then both doors too, then closes them
 * all; they close by themselves as soon as the car drives off.  Other
 * players see them open: roam_game.c sends the open parts with the car's
 * state (RC_OPEN_*, roam_core.h) and hands each friend's car and its parts
 * here, where they open on that car.
 *
 * The parts are the game's: its magazine and DVD cover shoot opens them on
 * the player's car out in the world (the shoot's D-pad screen,
 * UI_Showcase_DPAD.fng, 0x4B5AD0: 0x61F820 with the car's render info, the
 * part, 0 hood, 1 trunk, 2 left door, 3 right door, open or not, 1.0f; the
 * shoot's start loads the animations, 0x4CE770 -> 0x633A00, its end closes
 * them, 0x499F10).  Each car type has its own animations in
 * CARS\<car>\PARTS_ANIMATIONS.BIN (an animation bank, 32 KB at most):
 * <CAR>_ZAM_HOOD_REG, _HOOD_FRONT, the four split hoods, _TRUNK_REG and each
 * door's REG, SCISSOR, SUICIDE and WIDE1..4 (0x433AD0 picks one by the part
 * and its kind); all 31 player cars have them all, but for the three SUVs'
 * wide doors.  Drawing the car (0x620AE0) poses the parts by them: 0x433CD0
 * gives a part's state, 0x434190 / 0x434460 the doors' and 0x434610 the
 * hood's and trunk's pose; the lights' pass 0x615A10 reads the hood's and
 * the trunk's too.  Only the drawing: the car's body, physics and
 * collisions are the same with them open.
 *
 * A part's state is a node (0x28 bytes, made by 0x433A80) on its bank's list
 * (bank +0xC: the list's head, next and previous): +8 the speed (1.0f), +0xC
 * the part, +0x10 the state (1 closed, 2 opening, 0 open, 3 closing; 0x433D50
 * moves it on, timed on the game's clock [0x86518C], and the drawing ends an
 * opening or a closing), +0x14 its kind, +0x18/+0x1C when it began, +0x20 the
 * car type, +0x24 the hood's "style 22" mark.  The game keeps them per car
 * TYPE (0x433E20: one node per type and part, the bank found by the type's
 * hash, 0x433A30), so every car of a type opens with one, and it loads one
 * type's animations at a time (the car loader's one record, 0x61C600).  So
 * here each car has nodes of its own (made and moved by the game's own
 * 0x433A80 and 0x433D50, in a block of ours), put on its type's list while
 * the car is drawn (the overrides at the end) and taken off again after;
 * a car with none open is drawn as the game draws it.  The kind of each part
 * is picked as 0x61F820 picks it (so_kind), and the player's own car sounds
 * as the shoot's does (so_sound); friends' cars open without a sound.  Each
 * car type's animations are loaded for us (0x57CFB0, as 0x61C600 loads
 * them; kept for the session), once a car of that type opens; the game's
 * own load or unload of its record (0x61C600, 0x61C5B0: a cover shoot)
 * throws away every bank (0x438140 with 0), and ours are loaded again.
 *
 * The controller's D-pad down is the game's Legend (Career Status) as well:
 * in free roam with no screen over the HUD, a press goes to the game only
 * when it is let go within the second (as a short press then: showoff_pad,
 * from dinput_pad.c); held a second it is a step here instead when the car
 * stood at the press, and drift mode on or off when it was driving (drift.c
 * drift_pad, the keyboard's X).  Anywhere else the button is the game's as
 * before.  Nothing here runs or loads before the first press.
 *
 * Tests: tests/scripts/showoff_*.txt; "[showoff]" log lines. */
#include "cpu.h"
#include "kbd.h"
#include "gamepad.h"
#include "roam_core.h"            /* RC_OPEN_* */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

void dispatch_indirect(CPU *c, uint32_t addr);
long autotest_frame(void);

#define DIK_O          0x18u         /* in none of the game's key maps */
#define HOOK_RET       0x00581475u   /* the main-loop hook's return address, as guest calls see it */
#define G_FLOW         0x008654a4u   /* 6: in the world */
#define G_GAMEMODE     0x00850078u   /* 1 career, 8 Online, 0x200 LAN */
#define G_RACE         0x00890118u   /* the race coordinator: +0 state (4 racing, 6 paused), +0x24 racers */
#define G_RACEKIND     0x0089e7b0u   /* 0 free roam, 7 an outrun */
#define G_CAREER_RACE  0x00863470u   /* the career race being run (0 = free roam) */
#define G_FREEZE       0x00890084u   /* the world stands (photo mode) */
#define G_PLAYER1      0x008900acu   /* Player*: +4 its car, +0x9A4 its HUD */
#define G_FENG         0x008384c4u   /* +8 the package manager: +0xE4 the top package, +0xC its name */
#define G_TXT_ON       0x0083a9c0u   /* byte: the game's text entry takes the typing ... */
#define G_TXT_REC      0x0083a9c4u   /* ... for this record (0: none) */
#define G_BANKS        0x00827c9cu   /* the animation banks: a list, each entry 4 into its bank (0x433A30); the end 0x827C98 */
#define G_SOUND        0x0082b884u   /* +0x78 the world's sounds (0x481500 plays one), +0x90 1 in the front end */
#define FN_MALLOC      0x00575620u   /* cdecl (size) */
#define FN_NODE_INIT   0x00433a80u   /* thiscall node (type, speed, part, kind, style 22), ret 0x14 */
#define FN_NODE_GO     0x00433d50u   /* cdecl (node, 2 open / 3 close / 0 opened / 1 closed) */
#define FN_ATTR        0x0060fef0u   /* thiscall part (attribute's hash, 0) -> the attribute (+4 its value) */
#define FN_HASH        0x0043db50u   /* cdecl (string) */
#define FN_HASH2       0x0043db80u   /* cdecl (string, seed): the hash of the seed's string and this one */
#define FN_LOAD        0x0057cfb0u   /* cdecl (file, 0, 0, 1, callback, its argument) -> the resource file */
#define FN_UNLOAD      0x00580e90u   /* cdecl (resource file) */
#define FN_NOP         0x004022c0u   /* ret: the load's callback */
#define FN_SOUND       0x00481500u   /* thiscall world's sounds (which) */
#define S_ATTR_DOOR    0x007a8750u   /* the door style part's attribute: its doors' kind (1 scissor: wide on a wide body) */
#define S_ATTR_HOOD    0x007a8780u   /* a split hood's attribute: front split (else left and right) */
#define S_STYLE22      0x007a8770u   /* "_STYLE22_HOOD" */
#define S_STYLE22CF    0x007a875cu   /* "_STYLE22_HOOD_CF" */
#define CAR_RI         0x554u        /* car: its render info */
#define CAR_SPEED      0x42cu        /* car: its speed (m/s, forward) */
#define RI_RIDE        0x04u         /* render info: the ride */
#define RI_TYPE        0x08u         /* render info: its CarTypeInfo (+0 the name, +0xD0 its hash) */
#define RIDE_HOOD      0x5b4u        /* ride slot 9 */
#define RIDE_WIDE      0x5a8u        /* ride slot 6, the wide body */
#define RIDE_DOORS     0x810u        /* ride slot 0xA0, the door style */
#define NODE_SZ        0x28u
#define OPEN_MPS       1.0f          /* the car stands: its parts may open */
#define SHUT_MPS       2.0f          /* it drives off: they close */
#define HOLD_S         1.0           /* D-pad down held this long: a step */
#define TAP_S          0.12          /* ...let go before: the game's press, this long */
#define SO_FRIENDS     64            /* roam_game.c's slots */
#define SO_CARS        (1 + SO_FRIENDS)
#define SO_TYPES       48

static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n); }
static uint32_t rd32(uint32_t a){ return ok(a, 3) ? MR32(a) : 0u; }
static void cstr(uint32_t a, char *o, int n){
    int i = 0;
    for (; a && i + 1 < n && ok(a + (uint32_t)i, 0) && MR8(a + (uint32_t)i); i++) o[i] = (char)MR8(a + (uint32_t)i);
    o[i] = 0;
}
static double now_s(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
/* a guest function with ecx and stack arguments, the CPU as it was after (from the main-loop hook only) */
static uint32_t gcall(CPU *c, uint32_t fn, uint32_t ecx, int n, const uint32_t *arg){
    CPU save = *c;
    c->esp = (c->esp - 256u) & ~15u;
    for (int i = n - 1; i >= 0; i--) PUSH32(arg[i]);
    c->ecx = ecx;
    PUSH32(HOOK_RET);
    dispatch_indirect(c, fn);
    uint32_t r = c->eax;
    *c = save;
    return r;
}

/* a car: the player's (0) or a friend's (1 + roam_game.c's slot) */
typedef struct {
    uint32_t car, ri, type;           /* its car and render info, its type's hash */
    uint32_t node[4];                 /* each part's node (0: none yet), in our block */
    uint8_t  want;                    /* RC_OPEN_* its parts should be */
    uint8_t  has;                     /* RC_OPEN_* those we opened (their nodes opening or open) */
    uint8_t  live;                    /* a node of it is not closed: it goes on the list while drawn */
} Car;
void drift_pad(void);              /* drift.c: drift mode's switch, as X */
typedef struct { uint32_t type, file, name; uint8_t st; int tries; long since; } Bank;   /* st: 0 none, 1 loading, 2 in */
static struct {
    uint32_t mem;                     /* our block: SO_CARS x 4 nodes, then SO_TYPES file names of 64 bytes */
    Car car[SO_CARS];
    Bank bank[SO_TYPES]; int nbank;
    int nlive;                        /* cars with a node not closed */
    int step;                         /* the player's: 0 closed, 1 the trunk, 2 and the hood, 3 and the doors */
    int ostate; uint32_t kseq; int kinit;
    int gate, moving;                 /* the D-pad's hold is ours now (free roam, the HUD on top); the car drives */
    int held, fired, fires, drifts, hmove; double t0, tap_until;   /* hmove: the hold began driving (drift mode's) */
    int note;                         /* the last press that did nothing, logged once */
    int failed;                       /* the player's car type's animations can't be had */
} S;
static const uint8_t k_step[4] = { 0, RC_OPEN_TRUNK, RC_OPEN_TRUNK | RC_OPEN_HOOD, RC_OPEN_ALL };
static const char *const k_part[4] = { "hood", "trunk", "left door", "right door" };

/* the bank of a car type (0x433A30's walk; 0: its animations are not in) */
static uint32_t bank_of(uint32_t type){
    uint32_t e = rd32(G_BANKS), b = e ? e - 4u : 0;
    for (int n = 0; b && b != G_BANKS - 4u && n < 256 && ok(b, 0x28); n++){
        if (MR32(b + 0x24u) == type) return b;
        e = MR32(b + 4u); b = e ? e - 4u : 0;
    }
    return 0;
}

/* free roam's world: its own race, paused or not, not an outrun, a career race or an online one */
static int roam_world(void){
    uint32_t r = rd32(G_RACE), pl = rd32(G_PLAYER1);
    return rd32(G_FLOW) == 6u && ok(r, 0x28) && MR32(r + 0x24u) == 1u && (MR32(r) == 4u || MR32(r) == 6u) &&
           !rd32(G_CAREER_RACE) && rd32(G_RACEKIND) == 0u && !(rd32(G_GAMEMODE) & 0x208u) && pl && rd32(pl + 4u);
}
/* the HUD is the top package: no screen over it */
static int hud_on_top(void){
    uint32_t pl = rd32(G_PLAYER1), hud = pl ? rd32(pl + 0x9a4u) : 0, fe = rd32(G_FENG), mg = fe ? rd32(fe + 8u) : 0,
             top = mg ? rd32(mg + 0xe4u) : 0;
    char a[40], b[40];
    cstr(top ? rd32(top + 0xcu) : 0, a, sizeof a); cstr(hud ? rd32(hud + 0x10u) : 0, b, sizeof b);
    return a[0] && !strcasecmp(a, b);
}
static int typing(void){ return rd32(G_FLOW) != 6u || (mem_is_committed(G_TXT_REC + 3u) && MR8(G_TXT_ON) && MR32(G_TXT_REC)); }
/* a cover shoot is on (its screens on top: UI_ShowCase_Preview.fng, UI_Showcase_DPAD.fng): it opens and closes the
 * player's car's parts itself, on the game's list */
static int shoot_on(void){
    uint32_t fe = rd32(G_FENG), mg = fe ? rd32(fe + 8u) : 0, top = mg ? rd32(mg + 0xe4u) : 0;
    char a[40];
    cstr(top ? rd32(top + 0xcu) : 0, a, sizeof a);
    return !strncasecmp(a, "UI_Showcase", 11);
}

/* O presses since the last frame (a tap between frames counts) */
static int o_presses(void){
    uint32_t head = kbd_ev_head();
    if (!S.kinit){ S.kinit = 1; S.kseq = head; return 0; }
    int n = 0;
    for (; S.kseq != head; S.kseq++){
        KbdEv e;
        int r = kbd_ev_get(S.kseq, &e);
        if (r == 0) break;
        if (r > 0 && e.dik == DIK_O){ if (e.down && !S.ostate) n++; S.ostate = e.down; }
    }
    return n;
}

/* the controller's buttons as the game reads them (dinput_pad.c fill_state): D-pad down kept back in free roam, held a
 * second a step (the car stood at the press) or drift mode (it drove), the game's own press when let go before */
uint32_t showoff_pad(uint32_t b){
    const uint32_t D = 1u << PAD_DDOWN;
    double t = now_s();
    int down = (b & D) != 0;
    if (down && !S.held && S.gate){ S.held = 1; S.fired = 0; S.t0 = t; S.hmove = S.moving; }
    if (S.held){
        if (!down){ S.held = 0; if (!S.fired) S.tap_until = t + TAP_S; }
        else if (!S.fired && t - S.t0 >= HOLD_S){ S.fired = 1; if (S.hmove) S.drifts++; else S.fires++; }
        b &= ~D;
    }
    if (t < S.tap_until) b |= D;
    return b;
}

/* our block of guest memory (the first time a part opens) */
static int mem_ok(CPU *c){
    if (S.mem) return 1;
    uint32_t sz = SO_CARS * 4u * NODE_SZ + SO_TYPES * 64u;
    S.mem = gcall(c, FN_MALLOC, 0, 1, &sz);
    if (!S.mem){ fprintf(stderr, "[showoff] guest malloc failed\n"); return 0; }
    for (uint32_t i = 0; i < sz; i += 4) MR32(S.mem + i) = 0;
    return 1;
}
/* a car type's animations in (1), or on the way (0, loading them; -1: they can't be had) */
static int bank_ready(CPU *c, uint32_t ti){
    uint32_t type = ok(ti, 0xd4) ? MR32(ti + 0xd0u) : 0;
    if (!type) return -1;
    if (bank_of(type)) return 1;
    if (!roam_world()) return 0;                                       /* loads only out in free roam (sub_00438140) */
    Bank *k = 0;
    for (int i = 0; i < S.nbank && !k; i++) if (S.bank[i].type == type) k = &S.bank[i];
    if (!k){
        if (S.nbank == SO_TYPES || !mem_ok(c)) return -1;
        k = &S.bank[S.nbank]; memset(k, 0, sizeof *k); k->type = type;
        k->name = S.mem + SO_CARS * 4u * NODE_SZ + 64u * (uint32_t)S.nbank++;
    }
    long f = autotest_frame();
    if (k->st == 1 && f - k->since < 600) return 0;                    /* on the way (10 s at most) */
    char nm[24], path[64];
    cstr(ti, nm, sizeof nm);
    /* not in after 10 s, or its bank gone with the file still in: load it again.  The old file is left loaded, never
     * unloaded: the game's file unload (0x580E90 -> 0x439D70) takes the first bank of the file's car type off the list,
     * and with none there it calls through 0 (exit 134, 2026-11-02: ... car_apply); one that comes in late is a
     * second bank of the type, which 0x438140 (sub_00438140) deletes at the latest */
    if (k->st){
        if (k->file) fprintf(stderr, "[showoff] f=%ld %s: the part animations are not in: loading them again (the first file left loaded)\n", f, nm);
        k->file = 0; k->st = 0;
    }
    if (k->tries >= 3) return -1;
    snprintf(path, sizeof path, "CARS\\%s\\PARTS_ANIMATIONS.BIN", nm);
    for (int i = 0; i < 64; i++) MR8(k->name + (uint32_t)i) = (uint8_t)path[i < (int)sizeof path ? i : 63];
    MR8(k->name + 63u) = 0;
    uint32_t a[6] = { k->name, 0, 0, 1, FN_NOP, 0 };
    k->file = gcall(c, FN_LOAD, 0, 6, a);
    k->st = 1; k->since = f; k->tries++;
    fprintf(stderr, "[showoff] f=%ld %s: loading the car's part animations (%s)\n", f, nm, k->file ? "under way" : "FAILED");
    return 0;
}

/* a part's kind as 0x61F820 picks it: the hood by its hood part (split hoods: front, else left and right), the doors by
 * the door style part's attribute (scissor doors on a wide body: the wide body's), the trunk's the one; *mark: the
 * hood is the car's "style 22" hood */
static uint32_t so_kind(CPU *c, uint32_t ri, int part, uint32_t *mark){
    uint32_t ride = rd32(ri + RI_RIDE), ti = rd32(ri + RI_TYPE), type = ok(ti, 0xd4) ? MR32(ti + 0xd0u) : 0, kind = 0;
    *mark = 0;
    if (!ok(ride, 0x8f0)) return 0;
    if (part == 0){
        uint32_t h = rd32(ride + RIDE_HOOD);
        if (!ok(h, 0xe) || (MR8(h + 5u) & 0x1fu) != 5u) return 0;
        uint32_t s = S_ATTR_HOOD, ah = gcall(c, FN_HASH, 0, 1, &s), aa[2] = { ah, 0 }, at = gcall(c, FN_ATTR, h, 2, aa);
        kind = ok(at, 8) && MR32(at + 4u) ? 4u : 2u;
        uint32_t key = (uint32_t)MR16(h + 2u) << 16 | MR16(h), s1[2] = { S_STYLE22, type }, s2[2] = { S_STYLE22CF, type };
        *mark = key == gcall(c, FN_HASH2, 0, 2, s1) || key == gcall(c, FN_HASH2, 0, 2, s2);
        return kind;
    }
    if (part == 1) return 0;
    uint32_t d = rd32(ride + RIDE_DOORS);
    if (!ok(d, 0xe)) return 0;
    uint32_t s = S_ATTR_DOOR, ah = gcall(c, FN_HASH, 0, 1, &s), aa[2] = { ah, 0 }, at = gcall(c, FN_ATTR, d, 2, aa);
    if (!ok(at, 8)) return 0;
    kind = MR32(at + 4u);
    if (kind == 1u){
        uint32_t w = rd32(ride + RIDE_WIDE);
        if (ok(w, 6)){ uint8_t b = MR8(w + 5u); if ((b >> 5) && (b & 0x1fu)) kind = (b & 0x1fu) + 2u; }
    }
    return kind;
}
/* the player's car sounds as the shoot's does: the hood 3 / 2, the trunk 5 / 4, the doors (as one) 1 / 0 */
static void so_sound(CPU *c, int part, int open){
    if (part == 3) return;
    uint32_t snd = rd32(G_SOUND), w = snd ? rd32(snd + 0x78u) : 0;
    if (!w || rd32(snd + 0x90u) == 1u) return;
    uint32_t which = part == 0 ? (open ? 3u : 2u) : part == 1 ? (open ? 5u : 4u) : (open ? 1u : 0u);
    gcall(c, FN_SOUND, w, 1, &which);
}

static int live_of(const Car *k){
    for (int p = 0; p < 4; p++) if (k->node[p] && MR32(k->node[p] + 0x10u) != 1u) return 1;
    return 0;
}
static void count_live(void){ int n = 0; for (int i = 0; i < SO_CARS; i++) n += S.car[i].live = (uint8_t)live_of(&S.car[i]); S.nlive = n; }
/* a car gone (or another one in its place): its nodes closed at once, kept for the next */
static void car_reset(Car *k){
    for (int p = 0; p < 4; p++) if (k->node[p]) MR32(k->node[p] + 0x10u) = 1u;
    k->car = k->ri = k->type = 0; k->want = k->has = 0; k->live = 0;
}
/* the car's parts to what it wants: open those it should and has not, close those it has and should not (closing a
 * part never opened would open it: 0x433A80 makes a node opening).
 * A node is kept for the next car in its slot (car_reset), and it holds the car type it was made for (+0x20).  A part
 * turned the other way while it opens or closes (0x433D50 from 2 to 3, or 3 to 2) is timed by its animation: 0x433D10
 * looks up that type's bank (0x433A30) and hands it to 0x433AD0, which reads the bank's animation list with no check.
 * With that type's animations thrown away since (a garage, shop or race lobby: 0x438140), the bank is 0, the list
 * reads 0 and the game calls through 0 for ever: free roam froze, or stopped with error 134, in a crowd (2026-11-02
 * 22:47 to 11-03, stack sub_00433ad0 < sub_00433d10 < car_apply).  The log of one: slot 30's trunk opened on a 350Z,
 * the game threw the banks away, a Skyline came into the slot and opened the hood (a new node) and the trunk (the
 * 350Z's node, which the drawing never finds on the Skyline's list, so it stayed opening), and its trunk closing was
 * the 350Z's bank looked up.  So a node of another type is made anew when its part opens, and one whose type has no
 * bank closes at once.  Test: tests/scripts/showoff_stale.txt (NFSU2_TEST_SHOWOFF_STALE=1, script runs only: the
 * player's car's trunk node first made for a type with no animations, as a slot's earlier car leaves it). */
static void car_apply(CPU *c, Car *k, int sound, const char *who){
    if (k->want == k->has || !ok(k->ri, 0x2d48)) return;
    uint32_t ti = rd32(k->ri + RI_TYPE);
    int r = bank_ready(c, ti);
    if (r <= 0){
        if (r < 0 && k->has){ for (int p = 0; p < 4; p++) if (k->node[p]) MR32(k->node[p] + 0x10u) = 1u; k->has = 0; }
        if (r < 0){ k->want = 0; if (k == &S.car[0]) S.failed = 1; }
        return;
    }
    if (!mem_ok(c)) return;
    int idx = (int)(k - S.car);
    static int stale = -1;                                             /* test aid, script runs only */
    if (stale < 0){ const char *s = getenv("NFSU2_INPUT_SCRIPT"), *e = getenv("NFSU2_TEST_SHOWOFF_STALE"); stale = s && *s && e && *e == '1'; }
    if (stale == 1 && idx == 0 && !k->node[1]){
        uint32_t n = k->node[1] = S.mem + NODE_SZ, a[5] = { k->type ^ 0x5a5a5a5au, 0x3f800000u, 1u, 0u, 0u };
        gcall(c, FN_NODE_INIT, n, 5, a);
        MR32(n + 0x10u) = 1u; stale = 2;
        fprintf(stderr, "[showoff] f=%ld test: my car's trunk node is one of another car type, closed\n", autotest_frame());
    }
    for (int p = 0; p < 4; p++){
        uint8_t bit = (uint8_t)(1u << p);
        int w = (k->want & bit) != 0, h = (k->has & bit) != 0;
        if (w == h) continue;
        uint32_t mark = 0, kind = w ? so_kind(c, k->ri, p, &mark) : 0, n = k->node[p];
        if (w && (!n || MR32(n + 0x20u) != k->type)){
            n = k->node[p] = S.mem + ((uint32_t)idx * 4u + (uint32_t)p) * NODE_SZ;
            uint32_t a[5] = { k->type, 0x3f800000u, (uint32_t)p, kind, mark };
            gcall(c, FN_NODE_INIT, n, 5, a);                          /* opening, from now */
        } else if (!w && !bank_of(MR32(n + 0x20u))){
            MR32(n + 0x10u) = 1u;                                     /* closed at once: no animation to close by */
        } else {
            if (w){ MR32(n + 8u) = 0x3f800000u; MR32(n + 0x14u) = kind; MR8(n + 0x24u) = (uint8_t)mark; }
            uint32_t a[2] = { n, w ? 2u : 3u };
            gcall(c, FN_NODE_GO, 0, 2, a);
        }
        if (sound) so_sound(c, p, w);
        k->has ^= bit;
        fprintf(stderr, "[showoff] f=%ld %s: %s %s (kind %u)\n", autotest_frame(), who, k_part[p], w ? "opens" : "closes", kind);
    }
}
/* a car of ours to show: its car (0: none), what should be open */
static void car_set(Car *k, uint32_t car, uint8_t want){
    uint32_t ri = ok(car, 0x9e0) ? rd32(car + CAR_RI) : 0, ti = ok(ri, 0x2d48) ? rd32(ri + RI_TYPE) : 0;
    uint32_t type = ok(ti, 0xd4) ? MR32(ti + 0xd0u) : 0;
    if (!type){ if (k->car) car_reset(k); return; }
    if (k->car != car || k->ri != ri || k->type != type){ car_reset(k); k->car = car; k->ri = ri; k->type = type; }
    k->want = want & RC_OPEN_ALL;
}

/* roam_game.c: a friend's car shown in slot k (0: none) and the parts its player has open */
void showoff_car(CPU *c, int k, uint32_t car, uint8_t open){
    if (k < 0 || k >= SO_FRIENDS) return;
    Car *e = &S.car[1 + k];
    if (!car && !e->car) return;
    car_set(e, car, open);
    if (e->car){ char who[24]; snprintf(who, sizeof who, "slot %d", k); car_apply(c, e, 0, who); }
}
/* roam_game.c: the player's open parts, for friends (RC_OPEN_*) */
uint8_t showoff_mine(void){ return S.car[0].car ? S.car[0].want : 0; }

/* each frame, from the main-loop hook */
void showoff_frame(CPU *c){
    int world = roam_world(), top = world && hud_on_top() && !rd32(G_FREEZE);
    uint32_t pl = rd32(G_PLAYER1), car = pl ? rd32(pl + 4u) : 0;
    float v = ok(car, 0x9e0) ? fabsf(MRF32(car + CAR_SPEED)) : 0.f;
    if (!(v == v)) v = 0.f;
    int stands = top && v < OPEN_MPS;
    S.gate = top; S.moving = !stands;
    for (; S.drifts > 0; S.drifts--) drift_pad();
    int n = o_presses() * !typing(), fires = S.fires; S.fires = 0;
    for (int i = 0; i < n + fires; i++){
        const char *how = i < n ? "O" : "D-pad down held";
        if (!stands){
            if (!S.note) fprintf(stderr, "[showoff] f=%ld %s: nothing (%s)\n", autotest_frame(), how,
                                 !world ? "not in free roam" : !top ? "a screen over the HUD" : "the car moves");
            S.note = 1;
            continue;
        }
        S.note = 0;
        S.step = (S.step + 1) % 4;
        fprintf(stderr, "[showoff] f=%ld %s: step %d (%s)\n", autotest_frame(), how, S.step,
                S.step == 0 ? "all closed" : S.step == 1 ? "the trunk" : S.step == 2 ? "the trunk and the hood" : "the trunk, the hood and the doors");
    }
    int shoot = world && shoot_on();
    if (S.step && (!world || shoot || v > SHUT_MPS)){
        fprintf(stderr, "[showoff] f=%ld %s: all closed\n", autotest_frame(),
                !world ? "out of free roam" : shoot ? "a cover shoot" : "the car drives off");
        S.step = 0;
    }
    Car *me = &S.car[0];
    if (!world || shoot){ if (me->car) car_reset(me); }
    else if (S.step || me->car){ car_set(me, car, k_step[S.step]); car_apply(c, me, 1, "my car"); }
    if (S.failed){
        if (S.step) fprintf(stderr, "[showoff] f=%ld my car's part animations can't be had: closed\n", autotest_frame());
        S.failed = 0; S.step = 0;
    }
    count_live();
}

/* ---- the drawing: each car with its own nodes -------------------------------------------------------------------------
 * The car's render info is this (ecx) in both; its nodes go on its type's bank's list (the list's own, the game's,
 * set aside) for the call, then the list is as it was. */
static Car *drawn(uint32_t ri, uint32_t *bank){
    if (!S.nlive) return 0;
    for (int i = 0; i < SO_CARS; i++){
        Car *k = &S.car[i];
        if (k->live && k->ri == ri){ uint32_t b = bank_of(k->type); if (!b) return 0; *bank = b; return k; }
    }
    return 0;
}
static void put_on(Car *k, uint32_t b, uint32_t *keep){
    uint32_t head = b + 0xcu, prev = head;
    keep[0] = MR32(head); keep[1] = MR32(head + 4u);
    for (int p = 0; p < 4; p++){
        uint32_t n = k->node[p];
        if (!n) continue;
        MR32(prev) = n; MR32(n + 4u) = prev; prev = n;
    }
    MR32(prev) = head; MR32(head + 4u) = prev;
}
static void take_off(uint32_t b, const uint32_t *keep){ MR32(b + 0xcu) = keep[0]; MR32(b + 0x10u) = keep[1]; }

/* The trunk's audio (the layout, ride slot 0x22, and its 12 pieces, 0x23..0x2E, on the layout's places, render info
 * +0x2184): 0x620AE0 draws it only for a car whose racer record's driver is a human (car +0x14, +4 == 1: never a
 * friend's car, kind 2 or 4), and out in the world (not the front end, no cover shoot) each piece as an empty box of
 * its size (AUDIO_COMP_EMPTY_*, +0x2B74: what shows through a hatch's glass) instead of its own model (+0x2274).  The
 * cover shoot, which opens the trunk out in the world, has its flag (G_SHOWCASE, 0x499DB0) set: the pieces' own
 * models then, and the car at its first LOD (0x62118F).  So while a car's trunk is open (opening, open or closing)
 * near the camera, its drawing runs with that flag and a human's driver kind, both as they were again after.
 * Nothing else in 0x620AE0 or what it calls reads either (the flag's three reads are 0x62118F, 0x630883 and
 * 0x630C29; the kind's one 0x6306D4). */
#define G_SHOWCASE     0x00836352u   /* byte: a cover shoot is on (0x4CE770 sets it, 0x499FA0 clears it) */
#define G_VIEW1        0x00832e50u   /* eView 1: +0x40 its camera (+0x40 its position) */
#define CAR_REC        0x14u         /* car: its racer record (+4 the driver: 1 a human, 2 an AI racer, 3 traffic) */
#define CAR_POS        0x60u
#define TRUNK_NEAR_M   40.f          /* farther off the pieces are a few pixels: the game's boxes */
static uint32_t trunk_rec(const Car *k){
    static int game_way = -1;                                          /* test aid, script runs only: the game's drawing */
    if (game_way < 0){ const char *s = getenv("NFSU2_INPUT_SCRIPT"), *e = getenv("NFSU2_TEST_TRUNK_GAMEWAY"); game_way = s && *s && e && *e == '1'; }
    if (game_way) return 0;
    uint32_t n = k->node[1], car = k->car, rec = ok(car, 0x9e0) ? rd32(car + CAR_REC) : 0, cam = rd32(G_VIEW1 + 0x40u);
    if (!n || MR32(n + 0x10u) == 1u || !ok(rec, 8) || rd32(k->ri) != car || !ok(cam, 0x4c)) return 0;
    float dx = MRF32(car + CAR_POS) - MRF32(cam + 0x40u), dy = MRF32(car + CAR_POS + 4u) - MRF32(cam + 0x44u),
          dz = MRF32(car + CAR_POS + 8u) - MRF32(cam + 0x48u), d2 = dx * dx + dy * dy + dz * dz;
    return d2 == d2 && d2 < TRUNK_NEAR_M * TRUNK_NEAR_M ? rec : 0;
}

/* photo.c: a car photo mode does not draw (its Cars list); none where photo mode is not built in */
__attribute__((weak)) int photo_hides(uint32_t ri){ (void)ri; return 0; }
void sub_00620ae0_orig(CPU *c);
void sub_00620ae0(CPU *c){                       /* the car's drawing */
    if (photo_hides(c->ecx)){ POP32(c->eip); c->esp += 0x30u; return; }   /* (thiscall, ret 0x30) */
    uint32_t b = 0, keep[2]; Car *k = drawn(c->ecx, &b);
    if (!k){ sub_00620ae0_orig(c); return; }
    put_on(k, b, keep);
    uint32_t rec = trunk_rec(k), kind = 0; uint8_t shoot = 0;
    if (rec){ shoot = MR8(G_SHOWCASE); kind = MR32(rec + 4u); MR8(G_SHOWCASE) = 1; MR32(rec + 4u) = 1u; }
    sub_00620ae0_orig(c);
    if (rec){ MR8(G_SHOWCASE) = shoot; MR32(rec + 4u) = kind; }
    take_off(b, keep);
}
void sub_00615a10_orig(CPU *c);
void sub_00615a10(CPU *c){                       /* the car's lights (the hood's and the trunk's parts) */
    if (photo_hides(c->ecx)){ POP32(c->eip); c->esp += 0xcu; return; }    /* (its neon too; thiscall, ret 0xC) */
    uint32_t b = 0, keep[2]; Car *k = drawn(c->ecx, &b);
    if (!k){ sub_00615a10_orig(c); return; }
    put_on(k, b, keep);
    sub_00615a10_orig(c);
    take_off(b, keep);
}

/* ---- the game throws away the animation banks: ours first, and safely -------------------------------------------------
 * 0x438140 (cdecl, a car type's hash, 0: every bank) takes each bank of that type off the list (G_BANKS), lets go of its
 * animations (0x4380F0) and deletes it (its vtable's first, 1), then reads the next bank from the one it has just
 * deleted.  The game calls it with 0 only (from 0x61C63F, 0x61C5E2, 0x63B118, 0x637A05: the car loader's one record),
 * each time right after unloading that record's file, which takes its bank off: the list is empty and the walk never
 * runs.  Ours were still on it (a friend's open trunk is enough: any car lot, garage, cover shoot or race lobby
 * after it), so the walk deleted one and went on from freed memory: the heap keeps its free list there (heap.c, +0 and
 * +4 of a free block), the next "bank" was 0 and the game crashed calling through it (0x4380F0's null indirect call,
 * exit 134, from 18:05 UTC on 2026-11-02).  Here our files are unloaded first, the way the game unloads its own (so
 * their banks go as its do), and the walk reads the next bank before it deletes one.  Their cars' parts show closed
 * until their banks are in again (bank_ready, out in free roam only).  Test: tests/scripts/showoff_banks.txt;
 * NFSU2_TEST_BANKS_GAMEWAY=1 (script runs only) the game's way, for the A/B; NFSU2_TEST_BANKS_LOST=1 (script runs
 * only) the walk deletes ours with their files still in, as the live game's did when it got through
 * (tests/scripts/showoff_banks_lost.txt, bank_ready). */
#define FN_BANK_FREE   0x004380f0u   /* thiscall bank: lets go of its animations */
static void drop_ours(CPU *c){
    int n = 0;
    for (int i = 0; i < S.nbank; i++){
        Bank *k = &S.bank[i];
        if (!k->file || !bank_of(k->type)) continue;                   /* (still loading: not on the list yet) */
        uint32_t a = k->file;
        gcall(c, FN_UNLOAD, 0, 1, &a);
        k->file = 0; k->st = 0; k->tries = 0; n++;
    }
    if (!n) return;
    for (int i = 0; i < SO_CARS; i++){                                 /* their parts open again once the banks are back */
        Car *k = &S.car[i];
        for (int p = 0; p < 4; p++) if (k->node[p]) MR32(k->node[p] + 0x10u) = 1u;
        k->has = 0;
    }
    count_live();
    fprintf(stderr, "[showoff] f=%ld the game throws the part animations away: %d of ours unloaded first\n", autotest_frame(), n);
}
void sub_00438140_orig(CPU *c);
void sub_00438140(CPU *c){
    static int game_way = -1, lost = -1;                               /* test aids, script runs only: the game's walk; */
    if (game_way < 0){                                                 /* ours deleted with their files still in */
        const char *s = getenv("NFSU2_INPUT_SCRIPT"), *e = getenv("NFSU2_TEST_BANKS_GAMEWAY"), *l = getenv("NFSU2_TEST_BANKS_LOST");
        game_way = s && *s && e && *e == '1'; lost = s && *s && l && *l == '1';
    }
    if (game_way){ sub_00438140_orig(c); return; }
    uint32_t type = MR32(c->esp + 4u);
    if (!lost) drop_ours(c);
    uint32_t e = rd32(G_BANKS), b = e ? e - 4u : 0;
    for (int n = 0; b && b != G_BANKS - 4u && n < 4096 && ok(b, 0x28); n++){
        uint32_t ne = MR32(b + 4u), next = ne ? ne - 4u : 0;           /* read before the bank is deleted */
        if (!type || MR32(b + 0x24u) == type){
            uint32_t pv = MR32(b + 8u), nx = MR32(b + 4u), one = 1;
            MR32(pv) = nx; MR32(nx + 4u) = pv;
            gcall(c, FN_BANK_FREE, b, 0, 0);
            gcall(c, MR32(MR32(b)), b, 1, &one);
            fprintf(stderr, "[showoff] f=%ld a bank was still on the list: deleted\n", autotest_frame());
        }
        b = next;
    }
    POP32(c->eip);                                                    /* (cdecl) */
}
