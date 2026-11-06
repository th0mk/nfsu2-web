/* bf_uparts.c -- Bayview Forever's Stage 6 Unique Parts (BayviewForever.asi
 * v0.2.9, 0x712DB240..0x712DE200): after the career every OutrunsPerPart-th
 * outrun won with a model earns it a unique part reward, performance and
 * visual in turn; the game's own unique part challenge (a timed race to the
 * performance / body / graphics shop) is then on the map while that model is
 * driven (or, RaceToShop = 0, the shop opens the pick screen at once), and
 * the pick screen's pick or BACK uses the reward up.  DRAFT for the lead
 * (spec_uparts.md has the details).  The port's uniques.c (every unique
 * visual part in every car's shops after Caleb) stays as it is: these are
 * free picks on top of it.
 *
 * The game (SPEED2.EXE v1.2 NA):
 *  - 0x541320 (thiscall Career, bool won; ret 4, only from 0x5F84D7): an
 *    outrun's career result (coop.c overrides it; it calls
 *    uparts_outrun_result after the game's).
 *  - World challenges [0x838424] x [0x838420], 0x18 bytes: +2 the start
 *    marker's name, +8 hash (its SMS, 0x501310), +0xC the shop's hash (shop
 *    +0x38), +0x10 seconds, +0x14 type (1 unique visual: +0x15 the kind; 2
 *    unique performance: +0x15..+0x17 three categories 1..10).  Career +0x7148
 *    (0x861E88) the 32 open; +0xC00 (0x85B940) the runner: +4 float seconds,
 *    +8 the one run, +0x11C its state.
 *  - 0x5419C0 (thiscall 0x85B940): a challenge done; called only by the
 *    unique pick screen, from 0x56178E (BACK) and 0x561870 (a part picked).
 *  - 0x54CAF0 (thiscall Career, shop; ret 4, only from 0x499497): the player
 *    drives into a shop; with a challenge for that shop being run, the pick
 *    screen.
 *  - Career cars (0x7F8 bytes): +0x20 the model's hash, +0x2F4 + 4i the nine
 *    unique performance parts owned; 0x503AE0 the car type, 0x61BA30 (parts
 *    0x8A2B68: type, slot, 0, 4) the type's unique part of a slot.
 * Endless Career's 0x527D30 override calls uparts_after_rebuild() (BF's export
 * UniquePartsAfterRebuild) so a reward's challenge is among the open ones when
 * the markers are made.  State: "NFS Underground 2/<NAME>/BF/uparts.txt" (BF's
 * unique-parts.txt).  Settings: NFSU2_BF_FEATURES_UNIQUEPARTS,
 * NFSU2_BF_UNIQUEPARTS_{OUTRUNSPERPART,RACETOSHOP,MESSAGE}; it needs
 * NFSU2_BF_FEATURES_ENDLESSCAREER. */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void dispatch_indirect(CPU *c, uint32_t addr);
int fileio_resolve_save(const char *win, char *out, size_t outsz);
int fileio_resolve_save_w(const char *win, char *out, size_t outsz);
uint32_t garage_find(uint32_t db, uint32_t h);
int uniques_caleb_beaten(void);

#define G_CAREER       0x0085ad40u
#define G_STAGE        0x00862838u
#define G_GAMEMODE     0x00850078u   /* 1: career */
#define G_CAR_HANDLE   0x00863480u
#define G_CHAL         0x0085b940u   /* Career +0xC00: the challenge runner */
#define G_CHAL_TIME    0x0085b944u
#define G_CHAL_RUN     0x0085b948u
#define G_CHAL_STATE   0x0085ba5cu
#define G_CHAL_OPEN    0x00861e88u   /* 32 */
#define G_RACES_OPEN   0x00861f08u   /* 64 CareerRace*, +0x28 the start marker's hash */
#define G_OTHERS       0x00838424u
#define G_NOTHERS      0x00838420u
#define G_POOL         0x00838428u
#define G_INBOX        0x00860a60u
#define G_INBOX_N      0x00861e60u
#define G_INBOX_UNREAD 0x00861e64u
#define G_PROFILE      0x0083a9e0u
#define G_PARTS        0x008a2b68u
#define CARS0          0x0083ad90u
#define FN_REBUILD     0x00533310u
#define FN_SMS_OF      0x00501310u
#define FN_SMS_CHAL    0x005338c0u
#define FN_CARTYPE     0x00503ae0u   /* thiscall car -> type index, -1 */
#define FN_PART_FIRST  0x0061ba30u   /* thiscall parts (type, slot, 0, level); ret 0x10 */
#define RET_DECLINED   0x00561793u   /* after 0x56178E */
#define RET_PICKED     0x00561875u   /* after 0x561870 */
#define MAXC 64

static const struct { uint8_t kind; int slot; const char *name; } VK[5] = {
    { 0x0b, 9, "Hood" }, { 0x0d, -1, "Rims" }, { 0x0c, 12, "Spoiler" }, { 0x3e, -1, "Vinyl" }, { 0x16, 6, "Widebody Kit" },
};
static const int8_t PERF_IDX[11] = { -1, 3, 7, 5, 2, 1, 0, 8, 6, 4, 9 };   /* category byte -> car +0x2F4 index */
static const char *const PERF_NAME[10] = { "Weight Reduction", "Transmission", "Nitrous", "Engine", "Turbo",
                                           "Suspension", "Brakes", "ECU", "Tires", "(unused)" };

/* ---- settings ------------------------------------------------------------ */
static int s_on = -1, s_per, s_race, s_msg;
static int env_int(const char *k, int def, int lo, int hi){
    const char *e = getenv(k); int v = e && *e ? atoi(e) : def;
    return v < lo ? lo : v > hi ? hi : v;
}
static int env_bool(const char *k, int def){
    const char *e = getenv(k);
    if (!e || !*e) return def;
    return *e != '0' && strcasecmp(e, "off") && strcasecmp(e, "no") && strcasecmp(e, "false");
}
static void settings(void){
    if (s_on >= 0) return;
    s_on = env_bool("NFSU2_BF_FEATURES_UNIQUEPARTS", 0);
    s_per = env_int("NFSU2_BF_UNIQUEPARTS_OUTRUNSPERPART", 7, 0, 1000);
    s_race = env_bool("NFSU2_BF_UNIQUEPARTS_RACETOSHOP", 1);
    s_msg = env_bool("NFSU2_BF_UNIQUEPARTS_MESSAGE", 1);
    if (s_on && !env_bool("NFSU2_BF_FEATURES_ENDLESSCAREER", 0)){
        s_on = 0;
        fprintf(stderr, "[uparts] needs Endless Career (it puts the challenge on the map after its rebuild): off\n");
    }
    if (s_on) fprintf(stderr, "[uparts] on: a unique part every %d outrun win(s) per car%s, %s, message %s\n",
                      s_per, s_per ? "" : " (= never)", s_race ? "timed race to the shop" : "straight to the shop", s_msg ? "on" : "off");
}

/* ---- helpers --------------------------------------------------------------- */
static uint32_t gcall(CPU *c, uint32_t fn, uint32_t ecx, int n, const uint32_t *arg){
    CPU save = *c;
    c->esp = (c->esp - 256u) & ~15u;
    for (int i = n - 1; i >= 0; i--) PUSH32(arg[i]);
    c->ecx = ecx;
    PUSH32(fn);
    dispatch_indirect(c, fn);
    uint32_t r = c->eax;
    *c = save;
    return r;
}
static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n - 1u); }
static int post_career(void){ return (int32_t)MR32(G_STAGE) > 5 || uniques_caleb_beaten(); }
static uint32_t the_car(void){                      /* the career car's record, 0: none */
    uint32_t r = garage_find(CARS0, MR32(G_CAR_HANDLE));
    return r && ok(r, 0x7f8u) && MR8(r + 0x7f5u) ? r : 0u;
}
/* the first 8 challenges of a type (BF 0x712DBE00) */
static int challenges(uint8_t type, uint32_t out[8]){
    uint32_t t = MR32(G_OTHERS), n = MR32(G_NOTHERS); int k = 0;
    for (uint32_t i = 0; t && i < n && i < 1000 && k < 8; i++){ uint32_t e = t + i * 0x18u; if (MR8(e + 0x14u) == type) out[k++] = e; }
    return k;
}
static uint32_t visual_of(int kind){                 /* BF 0x712DBFE0 */
    uint32_t e[8]; int n = challenges(1, e);
    for (int i = 0; i < n; i++) if (MR8(e[i] + 0x15u) == VK[kind].kind) return e[i];
    return 0;
}
static int kind_of(uint32_t ev){ for (int k = 0; k < 5; k++) if (MR8(ev + 0x15u) == VK[k].kind) return k; return -1; }
static int car_can(CPU *c, uint32_t car, int kind){  /* BF 0x712DB240 */
    if (VK[kind].slot < 0) return 1;
    int32_t type = (int32_t)gcall(c, FN_CARTYPE, car, 0, 0);
    if (type < 0) return 0;
    uint32_t a[4] = { (uint32_t)type, (uint32_t)VK[kind].slot, 0, 4 };
    return gcall(c, FN_PART_FIRST, G_PARTS, 4, a) != 0;
}
static int perf_missing(uint32_t car){ int m = 0; for (uint32_t i = 0; i < 9; i++) m += MR32(car + 0x2f4u + 4u * i) == 0; return m; }
static uint32_t best_perf(uint32_t car){             /* BF 0x712DBF20 */
    uint32_t e[8], best = 0; int n = challenges(2, e), most = 0;
    for (int i = 0; i < n; i++){
        int miss = 0;
        for (uint32_t j = 0; j < 3; j++){
            uint8_t b = MR8(e[i] + 0x15u + j);
            if (b >= 1 && b <= 10 && PERF_IDX[b] >= 0 && PERF_IDX[b] <= 8 && MR32(car + 0x2f4u + 4u * (uint32_t)PERF_IDX[b]) == 0) miss++;
        }
        if (miss > most){ most = miss; best = e[i]; }
    }
    return best;
}
static uint32_t bhash(uint32_t s){ uint32_t h = 0xffffffffu; for (uint32_t i = 0; i < 256 && MR8(s + i); i++) h = h * 33u + MR8(s + i); return h; }
static uint32_t visual_free(CPU *c, uint32_t car, int next){   /* BF 0x712DC040 */
    for (int k = next; k < next + 5; k++){
        int kind = k % 5; uint32_t ev = visual_of(kind);
        if (!ev || !car_can(c, car, kind)) continue;
        uint32_t pool = MR32(G_POOL); if (!pool) continue;
        uint32_t h = bhash(pool + MR16(ev + 2u)); int busy = 0;
        for (uint32_t r = 0; r < 64; r++){ uint32_t race = MR32(G_RACES_OPEN + 4u * r); if (race && ok(race + 0x28u, 4) && MR32(race + 0x28u) == h){ busy = 1; break; } }
        if (!busy) return ev;
    }
    return 0;
}
static void open_challenge(uint32_t ev){             /* BF 0x712DBCC0 */
    int slot = -1;
    for (int j = 0; j < 32; j++){ uint32_t v = MR32(G_CHAL_OPEN + 4u * (uint32_t)j); if (v == ev) return; if (!v && slot < 0) slot = j; }
    if (slot < 0){ fprintf(stderr, "[uparts] the world challenge list is full; a challenge was not added\n"); return; }
    MR32(G_CHAL_OPEN + 4u * (uint32_t)slot) = ev;
}

/* ---- state (BF's unique-parts.txt) ------------------------------------------ */
static struct {
    char profile[33]; int n;
    uint32_t car[MAXC]; int wins[MAXC], perf[MAXC], vis[MAXC], earned[MAXC], next[MAXC];
} S;
static void live_profile(char out[33]){
    unsigned i = 0;
    for (; i < 32u; i++){ uint8_t ch = MR8(G_PROFILE + i); if (ch < 0x20u || ch == 0x7fu) break; out[i] = (char)ch; }
    out[i] = 0;
}
int bf_path(const char *profile, const char *file, char *out, size_t n, int write);   /* bf.c */
static int state_path(char *out, size_t n, int write){ return bf_path(S.profile, "uparts.txt", out, n, write); }
static int clampi(int v, int lo, int hi){ return v < lo ? lo : v > hi ? hi : v; }
static void state_load(const char *name){
    char path[1100], line[256];
    memset(&S, 0, sizeof S);
    snprintf(S.profile, sizeof S.profile, "%s", name);
    FILE *f = state_path(path, sizeof path, 0) ? fopen(path, "r") : 0;
    if (!f){ fprintf(stderr, "[uparts] profile \"%s\": no uparts.txt yet\n", name); return; }
    while (fgets(line, sizeof line, f) && S.n < MAXC){
        unsigned h; int w = 0, p = 0, v = 0, e = 0, x = 0;
        int got = sscanf(line, "car %x %d %d %d %d %d", &h, &w, &p, &v, &e, &x);
        if (got <= 2) continue;
        int i = S.n++;
        S.car[i] = h; S.wins[i] = clampi(w, 0, 1000000); S.perf[i] = clampi(p, 0, 9);
        if (got > 5){ S.vis[i] = clampi(v, 0, 5); S.earned[i] = clampi(e, 0, 1000000); S.next[i] = clampi(x, 0, 4); }
        else { S.vis[i] = 0; S.next[i] = 0; S.earned[i] = s_per > 0 ? clampi(S.wins[i] / s_per, 0, 1000000) : 0; }
    }
    fclose(f);
    fprintf(stderr, "[uparts] profile \"%s\": uparts.txt loaded (%d car(s))\n", name, S.n);
}
static void state_save(void){
    char path[1100], tmp[1110];
    if (!S.profile[0] || !state_path(path, sizeof path, 1)) return;
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *o = fopen(tmp, "w");
    if (!o){ fprintf(stderr, "[uparts] cannot write %s\n", tmp); return; }
    fprintf(o, "; BayviewForever unique parts for profile \"%s\". Written by the mod; edit only while the game is closed.\n"
               "; car <model hash> <post-game outrun wins> <performance parts waiting> <visual parts waiting>\n"
               ";     <rewards earned> <next visual kind: 0 hood, 1 rims, 2 spoiler, 3 vinyl, 4 widebody>\n", S.profile);
    for (int i = 0; i < S.n; i++)
        fprintf(o, "car %08X %d %d %d %d %d\n", S.car[i], S.wins[i], S.perf[i], S.vis[i], S.earned[i], S.next[i]);
    if (fclose(o) != 0 || rename(tmp, path) != 0) fprintf(stderr, "[uparts] COULD NOT replace %s\n", path);
}
static int profile_ok(void){
    char nm[33]; live_profile(nm);
    if (!nm[0] || strpbrk(nm, "\\/:*?\"<>|")) return 0;
    if (strcasecmp(nm, S.profile)) state_load(nm);
    return 1;
}
static int entry(uint32_t model, int create){        /* BF 0x712DB2B0 */
    for (int i = 0; i < S.n; i++) if (S.car[i] == model) return i;
    if (!create || !model || S.n >= MAXC) return -1;
    int i = S.n++;
    S.car[i] = model; S.wins[i] = S.perf[i] = S.vis[i] = S.earned[i] = S.next[i] = 0;
    return i;
}

/* the challenge's own unlock SMS, an older copy taken out of the inbox first */
static void sms(CPU *c, uint32_t ev){
    uint32_t h = MR32(ev + 8u), e = gcall(c, FN_SMS_OF, 0, 1, &h), n = MR32(G_INBOX_N);
    if (!e){ fprintf(stderr, "[uparts]   no message: the challenge's message is missing\n"); return; }
    for (uint32_t i = 0; n >= 1 && n <= 256 && i < n; i++){
        uint32_t m = G_INBOX + i * 20u;
        if (MR32(m) != e) continue;
        if (!MR8(m + 4u)) MR32(G_INBOX_UNREAD) -= 1u;
        for (uint32_t j = i; j + 1 < n; j++) for (uint32_t b = 0; b < 20u; b += 4u) MR32(G_INBOX + j * 20u + b) = MR32(G_INBOX + (j + 1) * 20u + b);
        for (uint32_t b = 0; b < 20u; b += 4u) MR32(G_INBOX + (n - 1) * 20u + b) = 0;
        MR32(G_INBOX_N) = n - 1u;
        break;
    }
    gcall(c, FN_SMS_CHAL, G_INBOX, 1, &ev);
    fprintf(stderr, "[uparts]   Rachel's message sent\n");
}

/* ---- the outrun's result (coop.c's sub_00541320 calls this after the game's) ---- */
void uparts_outrun_result(CPU *c, int won){
    settings();
    if (!s_on || !won || MR32(G_GAMEMODE) != 1u || !post_career() || !profile_ok()) return;
    uint32_t car = the_car();
    int i = car ? entry(MR32(car + 0x20u), 1) : -1;
    if (i < 0){ fprintf(stderr, "[uparts] outrun won, but the car driven is unknown: not counted\n"); return; }
    int wins = ++S.wins[i], missing = perf_missing(car), got = 0;      /* got: 1 performance, 2 visual */
    fprintf(stderr, "[uparts] outrun won with model %08X: %d post-game outrun win(s) (waiting: %d performance, %d visual; "
            "%d of 9 performance parts still to get)\n", S.car[i], wins, S.perf[i], S.vis[i], missing);
    if (s_per > 0 && wins % s_per == 0){
        int vis_ok = 0;
        if (S.vis[i] <= 4) for (int k = 0; k < 5 && !vis_ok; k++) vis_ok = visual_of(k) && car_can(c, car, k);
        int t = S.earned[i];
        if (vis_ok){ S.earned[i] = t + 1; got = (t & 1) || S.perf[i] >= missing ? 2 : 1; }
        else if (S.perf[i] < missing){ S.earned[i] = t + 1; got = 1; }
        if (got == 1) S.perf[i]++; else if (got == 2) S.vis[i]++;
        if (got) fprintf(stderr, "[uparts]   EARNED a unique %s part (%s's turn; waiting now: %d performance, %d visual)\n",
                         got == 1 ? "performance" : "visual", (t & 1) ? "visual" : "performance", S.perf[i], S.vis[i]);
        else fprintf(stderr, "[uparts]   %d wins, but the car already has every reward it can hold waiting: nothing earned\n", wins);
    }
    state_save();
    if (!got) return;
    if (!s_race){ fprintf(stderr, "[uparts]   drive into the %s with the car to pick it\n", got == 1 ? "performance shop" : "body shop (graphics shop for vinyls)"); return; }
    gcall(c, FN_REBUILD, G_CAREER, 0, 0);                  /* Endless Career's 0x527D30 -> uparts_after_rebuild */
    uint32_t ev = got == 1 ? best_perf(car) : visual_free(c, car, S.next[i]);
    if (ev && s_msg) sms(c, ev);
}

/* ---- Endless Career's rebuild (BF export UniquePartsAfterRebuild) ---------------- */
void uparts_after_rebuild(CPU *c){
    settings();
    if (!s_on || !s_race || !post_career() || MR32(G_CHAL_RUN) || !profile_ok()) return;
    uint32_t car = the_car(); int i = car ? entry(MR32(car + 0x20u), 0) : -1;
    if (i < 0) return;
    if (S.perf[i] > 0){ uint32_t ev = best_perf(car); if (ev) open_challenge(ev); }
    if (S.vis[i] > 0){
        uint32_t ev = visual_free(c, car, S.next[i]);
        if (ev) open_challenge(ev);
        else fprintf(stderr, "[uparts] a visual reward waits, but every start marker it could use has a race on it; it comes back when the races move\n");
    }
}

/* ---- a shop entered (RaceToShop = 0): the reward's challenge as just done there ---- */
void uparts_shop_entered(CPU *c){
    settings();
    uint32_t shop = MR32(c->esp + 4u);
    if (!s_on || s_race || c->ecx != G_CAREER || !shop || !ok(shop, 0x3cu) || MR32(G_CHAL_RUN) || !post_career() || !profile_ok()) return;
    uint32_t car = the_car(); int i = car ? entry(MR32(car + 0x20u), 0) : -1;
    if (i < 0) return;
    uint32_t h = MR32(shop + 0x38u), ev = 0;
    if (S.perf[i] > 0){ uint32_t e = best_perf(car); if (e && MR32(e + 0xcu) == h) ev = e; }
    for (int k = 0; !ev && S.vis[i] > 0 && k < 5; k++){
        int kind = (S.next[i] + k) % 5; uint32_t e = visual_of(kind);
        if (e && car_can(c, car, kind) && MR32(e + 0xcu) == h) ev = e;
    }
    if (!ev) return;
    MR8(G_CHAL) = 0; MR32(G_CHAL_RUN) = ev; MR32(G_CHAL_STATE) = 1u; MRF32(G_CHAL_TIME) = (float)(int32_t)MR32(ev + 0x10u);
    fprintf(stderr, "[uparts] shop with model %08X: the unique %s pick screen\n", S.car[i], MR8(ev + 0x14u) == 2 ? "performance part" : "visual part");
}
#ifdef BF_UPARTS_OWNS_54CAF0
void sub_0054caf0_orig(CPU *c);
void sub_0054caf0(CPU *c){ uparts_shop_entered(c); sub_0054caf0_orig(c); }
#endif

/* ---- the pick screen's part (for the log) ---- */
static int g_granted = -1;
void sub_00521430_orig(CPU *c);
void sub_00521430(CPU *c){ g_granted = (int)MR32(c->esp + 4u); sub_00521430_orig(c); }

/* ---- a challenge done: the pick screen picked (0x561870) or BACK (0x56178E) ---- */
void sub_005419c0_orig(CPU *c);
void sub_005419c0(CPU *c){
    settings();
    uint32_t ret = MR32(c->esp), ev = MR32(G_CHAL_RUN);
    int picked = ret == RET_PICKED;
    uint8_t type = ev && ok(ev, 0x18u) ? MR8(ev + 0x14u) : 0;
    if (s_on && c->ecx == G_CHAL && (ret == RET_PICKED || ret == RET_DECLINED) && (type == 1 || type == 2) && post_career() && profile_ok()){
        uint32_t car = the_car(); int i = car ? entry(MR32(car + 0x20u), 0) : -1;
        if (i >= 0 && (type == 2 ? S.perf[i] : S.vis[i]) > 0){
            if (type == 2) S.perf[i]--;
            else { S.vis[i]--; int k = kind_of(ev); S.next[i] = k < 0 ? 0 : (k + 1) % 5; }
            state_save();
            fprintf(stderr, "[uparts] challenge done: %s (%s); %d performance, %d visual still waiting\n",
                    picked ? "picked" : "declined (BACK), the reward is used up",
                    type == 2 ? (picked && g_granted >= 0 && g_granted <= 9 ? PERF_NAME[g_granted] : "performance part")
                              : (kind_of(ev) >= 0 ? VK[kind_of(ev)].name : "visual part"), S.perf[i], S.vis[i]);
        } else if (i >= 0) fprintf(stderr, "[uparts] challenge done, but no reward was waiting for it\n");
    }
    g_granted = -1;
    sub_005419c0_orig(c);
}
