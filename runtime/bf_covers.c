/* bf_covers.c -- Bayview Forever's Stage 6 Covers (BayviewForever.asi v0.2.9,
 * unit 0x712C1000..0x712C66C0): after the career every career race won counts
 * for the model driven; every MagazineEveryWins-th win earns that model a
 * magazine cover, every DVDEveryWins-th a DVD cover (the one shot longest ago
 * that waits for nobody), at most MaxWaitingPerCar waiting per model; the
 * shoot (or, for a magazine, the race to the photographer) is open only while
 * that model is driven; the pay is CashPerStar x the car's stars; and every
 * version of a cover picture is kept and browsed with UP/DOWN in Career >
 * Magazines / DVDs.  DRAFT for the lead (spec_covers.md has the details).
 *
 * The game (SPEED2.EXE v1.2 NA):
 *  - Career (0x85AD40) 0x54C940 "Career_ApplyRaceResult" (thiscall, no args):
 *    with a result pending (+0xD25, not +0xD28) it asks 0x5287A0 whether the
 *    player won; a win sets +0xD28 (0x85BA68) and pays/marks the race (0x541420).
 *  - Showcases (cover shoots) [0x83840C] x [0x838408], 0x40 bytes: +0x20 hash,
 *    +0x24 kind (1/3 magazine, 2/4 DVD), +0x26 s16 base cash, +0x28 the hash the
 *    gallery puts in 0x837FD8 when a cover is opened (0x53DE93).  A cover's
 *    name "Magazine n"/"DVD n" counts its kind in table order (0x510B80); its
 *    picture is the memory-card file "<profile> <cover>\<profile> <cover>".
 *  - 0x501150 "Showcase_IsOffered" (thiscall Career+0x4210, showcase; ret 4):
 *    the career's event record +4 == 2 and +6 != 1 (done covers: never).  Its
 *    only callers (the markers 0x5334A1, the engage 0x52793C, 0x53319B,
 *    0x532AED) are what BF hooks.
 *  - 0x533680 "Showcase_MarkDone" (thiscall 0x85EF50; +8 the showcase's hash):
 *    a photo taken (0x498EBF, 0x4CEEB2); the new picture is written after it.
 *  - 0x541210 (thiscall Career, showcase; ret 4): the pay, +0x26 plus
 *    (int)(stage info +4 x 0x528410's stars + 0.005).
 *  - The cover viewer (constructor 0x553BE0, vtable 0x79ACE8), Notify 0x565B50
 *    (slot 2; thiscall msg, a, b, c; ret 0x10): 0x35F8620B makes the picture
 *    (ScreenCap 0x832DAC: +8 pixels, +0xC texture) and loads the file,
 *    0x42317A19 loaded (0x488B10 uploads +8 to the texture), 0x2B46FE08 failed,
 *    0x911AB364 back, 0xC98356BA every frame.  The pads: [0x8384C4] +8, 0x88
 *    bytes each (+4 buttons: bit 0 up, bit 1 down), +0xD4 how many.
 *  - World challenges ("other events") [0x838424] x [0x838420], 0x18 bytes:
 *    type 4 (+0x14) leads to a magazine showcase (+0xC its hash), +0x10 its
 *    seconds; Career +0x7148 (0x861E88): the 32 open now; +0xC08 (0x85B948)
 *    the one being run.
 * Here each of those is an override (same callers as BF's patches); Endless
 * Career's 0x527D30 override calls covers_after_rebuild() (BF's export
 * CoversAfterRebuild) to put a waiting magazine's challenge into the open
 * list before the markers are made.  The state lives beside the profile,
 * "NFS Underground 2/<NAME>/BF/covers.txt" (BF's covers-state.txt format), the
 * old pictures in "NFS Underground 2/<NAME>/BF/archive/<COVER>/NNN.cover" (BF's
 * NNN.cover: the game's file byte for byte), so they go with the profile
 * (renamed, deleted, backed up), and the game's cover folders keep only the
 * game's own file.  Settings: NFSU2_BF_* (BayviewForever.ini's
 * [Features] Covers, [Covers.Earn], [Covers.Viewer], [Covers.Log]). */
#include "cpu.h"
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>

void dispatch_indirect(CPU *c, uint32_t addr);
uint32_t gheap_alloc(int heap, uint32_t n, int zero);
int fileio_resolve_save(const char *win, char *out, size_t outsz);
int fileio_resolve_save_w(const char *win, char *out, size_t outsz);
uint32_t garage_find(uint32_t db, uint32_t h);      /* garage.c: a career car by handle, slots 6-10 too */
int uniques_caleb_beaten(void);                     /* uniques.c */
const char *language_name(void);

#define G_CAREER       0x0085ad40u
#define G_STAGE        0x00862838u   /* Career +0x7AF8 */
#define G_BANK         0x00861e74u   /* Career +0x7134 */
#define G_RACE_RUN     0x00863470u   /* Career +0x8730 */
#define G_CAR_HANDLE   0x00863480u   /* Career +0x8740 */
#define G_RES_PENDING  0x0085ba65u   /* Career +0xD25 */
#define G_RES_WON      0x0085ba68u   /* Career +0xD28 */
#define G_SHOW_CUR     0x0085ef58u   /* Career +0x4210 +8: the showcase being shot */
#define G_CHAL_RUN     0x0085b948u   /* Career +0xC08: the world challenge being run */
#define G_CHAL_OPEN    0x00861e88u   /* Career +0x7148: 32 open world challenges */
#define G_STAGE_REC    0x00862018u   /* Career +0x72D8: 0x104 a stage; +0x100 its info */
#define G_SHOWS        0x0083840cu
#define G_NSHOWS       0x00838408u
#define G_OTHERS       0x00838424u
#define G_NOTHERS      0x00838420u
#define G_SMS_TAB      0x00838454u
#define G_SMS_N        0x00838450u
#define G_INBOX        0x00860a60u
#define G_INBOX_N      0x00861e60u
#define G_INBOX_UNREAD 0x00861e64u
#define G_PROFILE      0x0083a9e0u   /* the profile's name, 32 bytes */
#define G_DESC         0x00837fd8u   /* the cover the gallery opened (showcase +0x28) */
#define G_SCREENCAP    0x00832dacu   /* +8 pixels, +0xC texture */
#define G_FEMGR        0x008384c4u
#define CARS0          0x0083ad90u
#define FN_REBUILD     0x00533310u   /* thiscall Career: markers made again (0x527D30 first) */
#define FN_RATING      0x00528410u   /* thiscall Career -> st0 stars */
#define FN_SMS_CHAL    0x005338c0u   /* thiscall inbox (other event); ret 4 */
#define FN_SMS_DVD     0x005299b0u   /* thiscall inbox (n); ret 4 */
#define FN_SMS_OF      0x00501310u   /* cdecl (hash) -> SMS entry */
#define FN_TEXT        0x004ffa80u   /* cdecl (hash) -> the game's text */
#define FN_FE_FIND     0x005379c0u   /* cdecl (package name, object hash) -> object */
#define FN_FE_SETSTR   0x00535fe0u   /* thiscall object (char*); ret 4 */
#define FN_UPLOAD      0x00488b10u   /* thiscall ScreenCap: pixels to the texture */
#define MSG_OPEN       0x35f8620bu
#define MSG_LOADED     0x42317a19u
#define MSG_FAILED     0x2b46fe08u
#define MSG_BACK       0x911ab364u
#define MSG_FRAME      0xc98356bau
#define PIC_SIZE       0x100024u
#define PIC_DATA       0x24u
#define MAXS           64
#define MAXV           512

/* ---- settings ------------------------------------------------------------ */
static int s_on = -1, s_mag, s_dvd, s_maxw, s_msg, s_chal, s_counter, s_cash, s_all;
__attribute__((unused)) static int s_detail;   /* [Covers.Log] Detailed: read, no detailed lines yet */
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
    s_on = env_bool("NFSU2_BF_FEATURES_COVERS", 0);
    s_mag = env_int("NFSU2_BF_COVERS_EARN_MAGAZINEEVERYWINS", 5, 0, 1000);
    s_dvd = env_int("NFSU2_BF_COVERS_EARN_DVDEVERYWINS", 10, 0, 1000);
    s_maxw = env_int("NFSU2_BF_COVERS_EARN_MAXWAITINGPERCAR", 3, 1, 30);
    s_msg = env_bool("NFSU2_BF_COVERS_EARN_MESSAGE", 1);
    s_chal = env_bool("NFSU2_BF_COVERS_EARN_MAGAZINECHALLENGE", 1);
    s_cash = env_int("NFSU2_BF_COVERS_EARN_CASHPERSTAR", 500, 250, 1000);   /* the port: 250 to 1000 (the mod 0 to 3000) */
    s_counter = env_int("NFSU2_BF_COVERS_VIEWER_COUNTER", 2, 0, 2);
    s_detail = env_bool("NFSU2_BF_COVERS_LOG_DETAILED", 0);
    s_all = env_bool("NFSU2_BF_COVERS_LOG_TESTBROWSEALL", env_bool("NFSU2_BF_COVERS_VIEWER_TESTBROWSEALL", 0));
    if (s_chal && !env_bool("NFSU2_BF_FEATURES_ENDLESSCAREER", 0)){
        s_chal = 0;
        fprintf(stderr, "[covers] MagazineChallenge needs Endless Career (it puts the challenge on the map); magazines go straight to the shoot\n");
    }
    if (s_on)
        fprintf(stderr, "[covers] on: magazine every %d win(s) (%s), DVD every %d, at most %d waiting per car, message %s, "
                "pay %s, counter %d\n", s_mag, s_chal ? "race to the photographer" : "straight to the shoot", s_dvd, s_maxw,
                s_msg ? "on" : "off", s_cash ? "per star" : "the game's", s_counter);
}

/* ---- guest helpers ------------------------------------------------------- */
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
__attribute__((unused)) static uint32_t bhash(const char *s){ uint32_t h = 0xffffffffu; while (*s) h = h * 33u + (uint8_t)*s++; return h; }
static int post_career(void){ return (int32_t)MR32(G_STAGE) > 5 || uniques_caleb_beaten(); }

static uint32_t nshows(void){ uint32_t n = MR32(G_NSHOWS); return MR32(G_SHOWS) && n >= 1 && n <= 64 ? n : 0; }
static uint32_t show_at(uint32_t i){ return MR32(G_SHOWS) + i * 0x40u; }
static int is_mag(uint32_t q){ return (MR8(q + 0x24u) & ~2u) == 1u; }
static int is_dvd(uint32_t q){ return ((MR8(q + 0x24u) - 2u) & 0xfdu) == 0u; }
static int show_by(uint32_t off, uint32_t h){
    for (uint32_t i = 0, n = nshows(); i < n; i++) if (MR32(show_at(i) + off) == h) return (int)i;
    return -1;
}
/* "Magazine n" / "DVD n" (0x510B80's numbering) */
static int cover_name(int i, char *out, size_t sz){
    out[0] = 0;
    if (i < 0 || (uint32_t)i >= nshows()) return 0;
    uint32_t q = show_at((uint32_t)i); int k = 0;
    for (int j = 0; j <= i; j++){ uint32_t p = show_at((uint32_t)j); if (is_mag(q) ? is_mag(p) : is_dvd(p)) k++; }
    if (is_mag(q)) snprintf(out, sz, "Magazine %d", k);
    else if (is_dvd(q)) snprintf(out, sz, "DVD %d", k);
    return out[0] != 0;
}
/* the type-4 world challenge that leads to showcase hash h */
static uint32_t challenge_of(uint32_t h){
    uint32_t t = MR32(G_OTHERS), n = MR32(G_NOTHERS);
    for (uint32_t i = 0; t && i < n && i < 1000; i++){ uint32_t e = t + i * 0x18u; if (MR8(e + 0x14u) == 4 && MR32(e + 0xcu) == h) return e; }
    return 0;
}
static int has_challenge(uint32_t q){ return s_chal && is_mag(q) && challenge_of(MR32(q + 0x20u)); }
/* the model the career drives (car +0x20), 0: none */
static uint32_t car_model(void){
    uint32_t r = garage_find(CARS0, MR32(G_CAR_HANDLE));
    return r && ok(r, 0x7f8u) && MR8(r + 0x7f5u) ? MR32(r + 0x20u) : 0u;
}

/* ---- state (BF's covers-state.txt) --------------------------------------- */
static struct {
    char profile[33];
    int seq, ncar, nshow;
    uint32_t car[MAXS]; int wins[MAXS];
    uint32_t show[MAXS], waiting[MAXS]; int shot[MAXS];
} S;
static void live_profile(char out[33]){
    unsigned i = 0;
    for (; i < 32u; i++){ uint8_t ch = MR8(G_PROFILE + i); if (ch < 0x20u || ch == 0x7fu) break; out[i] = (char)ch; }
    out[i] = 0;
}
int bf_path(const char *profile, const char *file, char *out, size_t n, int write);   /* bf.c */
static int state_path(char *out, size_t n, int write){ return bf_path(S.profile, "covers.txt", out, n, write); }
static int show_entry(uint32_t h, int add){
    for (int i = 0; i < S.nshow; i++) if (S.show[i] == h) return i;
    if (!add || S.nshow >= MAXS) return -1;
    S.show[S.nshow] = h; S.waiting[S.nshow] = 0; S.shot[S.nshow] = 0;
    return S.nshow++;
}
static void state_load(const char *name){
    char path[1100], line[256];
    memset(&S, 0, sizeof S);
    snprintf(S.profile, sizeof S.profile, "%s", name);
    FILE *f = state_path(path, sizeof path, 0) ? fopen(path, "r") : 0;
    if (!f){ fprintf(stderr, "[covers] profile \"%s\": no covers.txt yet\n", name); return; }
    while (fgets(line, sizeof line, f)){
        unsigned a, b; int v;
        if (sscanf(line, "seq %d", &v) == 1) S.seq = v;
        else if (sscanf(line, "wins %x %d", &a, &v) == 2){ if (S.ncar < MAXS){ S.car[S.ncar] = a; S.wins[S.ncar++] = v; } }
        else if (sscanf(line, "waiting %x %x", &a, &b) == 2){ int k = show_entry(a, 1); if (k >= 0) S.waiting[k] = b; }
        else if (sscanf(line, "shot %x %d", &a, &v) == 2){ int k = show_entry(a, 1); if (k >= 0) S.shot[k] = v; }
    }
    fclose(f);
    int w = 0; for (int i = 0; i < S.nshow; i++) w += S.waiting[i] != 0;
    fprintf(stderr, "[covers] profile \"%s\": covers.txt loaded (%d car(s) with wins, %d cover(s) waiting)\n", name, S.ncar, w);
}
static void state_save(void){
    char path[1100], tmp[1110], nm[32];
    if (!S.profile[0] || !state_path(path, sizeof path, 1)) return;
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *o = fopen(tmp, "w");
    if (!o){ fprintf(stderr, "[covers] cannot write %s\n", tmp); return; }
    fprintf(o, "; NFSU2 Covers state for profile \"%s\". Written by the plugin; edit only while the game is closed.\n", S.profile);
    fprintf(o, "seq %d\n", S.seq);
    for (int i = 0; i < S.ncar; i++) fprintf(o, "wins %08X %d\n", S.car[i], S.wins[i]);
    for (int i = 0; i < S.nshow; i++){
        cover_name(show_by(0x20u, S.show[i]), nm, sizeof nm);
        if (S.waiting[i]) fprintf(o, "waiting %08X %08X ; %s\n", S.show[i], S.waiting[i], nm[0] ? nm : "unknown");
        if (S.shot[i]) fprintf(o, "shot %08X %d ; %s\n", S.show[i], S.shot[i], nm[0] ? nm : "unknown");
    }
    if (fclose(o) != 0 || rename(tmp, path) != 0) fprintf(stderr, "[covers] COULD NOT replace %s\n", path);
}
/* BF 0x712C1950: a usable profile name, its state loaded (again when it changed) */
static int profile_ok(void){
    char nm[33]; live_profile(nm);
    if (!nm[0] || strpbrk(nm, "\\/:*?\"<>|")) return 0;
    if (strcasecmp(nm, S.profile)) state_load(nm);
    return 1;
}

/* ---- earning (BF 0x712C29C0) --------------------------------------------- */
static int earn(int dvd, uint32_t model){
    int waiting = 0;
    for (int i = 0; i < S.nshow; i++) waiting += S.waiting[i] == model;
    if (waiting >= s_maxw){
        fprintf(stderr, "[covers]   the car already has %d cover(s) waiting (MaxWaitingPerCar): no new %s\n", waiting, dvd ? "DVD" : "magazine");
        return -1;
    }
    int best = -1, bseq = 0;
    for (uint32_t i = 0, n = nshows(); i < n; i++){
        uint32_t q = show_at(i);
        if (dvd ? !is_dvd(q) : !is_mag(q)) continue;
        int k = show_entry(MR32(q + 0x20u), 0), seq = k >= 0 ? S.shot[k] : 0;
        if (k >= 0 && S.waiting[k]) continue;
        if (best < 0 || seq < bseq){ best = (int)i; bseq = seq; }
    }
    if (best < 0){ fprintf(stderr, "[covers]   every %s cover already waits for some car: none earned\n", dvd ? "DVD" : "magazine"); return -1; }
    int k = show_entry(MR32(show_at((uint32_t)best) + 0x20u), 1);
    if (k < 0) return -1;
    S.waiting[k] = model;
    char nm[32]; cover_name(best, nm, sizeof nm);
    fprintf(stderr, "[covers]   EARNED %s (%s) for model %08X; it shows on the map while that car is driven\n",
            nm, has_challenge(show_at((uint32_t)best)) ? "race to the photographer" : "marker", model);
    return best;
}
/* Rachel's message for showcase i (BF 0x712C4110): the challenge's own unlock SMS for a magazine,
 * SMS_DVD_COVER_<n> for a DVD; one already in the inbox is taken out first so it comes as new */
static void inbox_take(uint32_t entry){
    uint32_t n = MR32(G_INBOX_N);
    if (!entry || n < 1 || n > 256) return;
    for (uint32_t i = 0; i < n; i++){
        uint32_t m = G_INBOX + i * 20u;
        if (MR32(m) != entry) continue;
        if (!MR8(m + 4u)) MR32(G_INBOX_UNREAD) -= 1u;
        for (uint32_t j = i; j + 1 < n; j++) for (uint32_t b = 0; b < 20u; b += 4u) MR32(G_INBOX + j * 20u + b) = MR32(G_INBOX + (j + 1) * 20u + b);
        for (uint32_t b = 0; b < 20u; b += 4u) MR32(G_INBOX + (n - 1) * 20u + b) = 0;
        MR32(G_INBOX_N) = n - 1u;
        return;
    }
}
static void sms(CPU *c, int i){
    uint32_t q = show_at((uint32_t)i); char nm[32]; cover_name(i, nm, sizeof nm);
    if (is_mag(q)){
        uint32_t ev = challenge_of(MR32(q + 0x20u)), h, e;
        if (!ev){ fprintf(stderr, "[covers]   no message for %s (no challenge leads to it)\n", nm); return; }
        h = MR32(ev + 8u); e = gcall(c, FN_SMS_OF, 0, 1, &h);
        if (!e){ fprintf(stderr, "[covers]   no message for %s\n", nm); return; }
        inbox_take(e);
        gcall(c, FN_SMS_CHAL, G_INBOX, 1, &ev);
    } else {
        uint32_t n = (uint32_t)atoi(nm + 4), t = MR32(G_SMS_TAB), cnt = MR32(G_SMS_N), e = 0;
        for (uint32_t k = 0; t && k < cnt && k < 1000; k++){ uint32_t r = t + k * 20u; if (MR8(r + 2u) == 0x0e && MR32(r + 8u) == n){ e = r; break; } }
        if (!e){ fprintf(stderr, "[covers]   no message for %s (no DVDUnlock message %u)\n", nm, n); return; }
        inbox_take(e);
        gcall(c, FN_SMS_DVD, G_INBOX, 1, &n);
    }
    fprintf(stderr, "[covers]   Rachel's message for %s sent\n", nm);
}

/* ---- 1. race result: Career_ApplyRaceResult ------------------------------ */
void sub_0054c940_orig(CPU *c);
void sub_0054c940(CPU *c){
    settings();
    int mine = s_on && c->ecx == G_CAREER && MR8(G_RES_PENDING) && !MR8(G_RES_WON);
    uint32_t race = MR32(G_RACE_RUN);
    sub_0054c940_orig(c);
    if (!mine || !MR8(G_RES_WON)) return;
    if (!post_career()) return;
    if (!profile_ok()) return;
    uint32_t model = car_model();
    if (!model){ fprintf(stderr, "[covers] race won, but the car driven is unknown: not counted\n"); return; }
    int k = 0;
    while (k < S.ncar && S.car[k] != model) k++;
    if (k == S.ncar){ if (S.ncar >= MAXS) return; S.car[S.ncar] = model; S.wins[S.ncar++] = 0; }
    int wins = ++S.wins[k];
    fprintf(stderr, "[covers] race %08X won with model %08X: %d post-game win(s) with it\n", race && ok(race + 8u, 4) ? MR32(race + 8u) : 0u, model, wins);
    int dvd = s_dvd > 0 && wins % s_dvd == 0 ? earn(1, model) : -1;
    int mag = s_mag > 0 && wins % s_mag == 0 ? earn(0, model) : -1;
    state_save();
    if (dvd < 0 && mag < 0) return;
    uint32_t c0 = c->ecx;
    gcall(c, FN_REBUILD, G_CAREER, 0, 0);
    c->ecx = c0;
    if (s_msg){ if (dvd >= 0) sms(c, dvd); if (mag >= 0) sms(c, mag); }
}

/* ---- 2. cover offered: Showcase_IsOffered -------------------------------- */
void sub_00501150_orig(CPU *c);
void sub_00501150(CPU *c){
    settings();
    uint32_t q = MR32(c->esp + 4u);
    int n = (int)nshows();
    if (!s_on || !n || q < show_at(0) || q >= show_at((uint32_t)n) || (q - show_at(0)) % 0x40u || !post_career() || !profile_ok()){
        sub_00501150_orig(c); return; }
    uint32_t h = MR32(q + 0x20u), run = MR32(G_CHAL_RUN);
    int yes = -1;
    if (run && ok(run, 0x18u) && MR32(run + 0xcu) == h) yes = 1;          /* its challenge is running */
    else {
        int k = show_entry(h, 0);
        if (k >= 0 && S.waiting[k] && S.waiting[k] == car_model() && !has_challenge(q)) yes = 1;
    }
    if (yes < 0){ sub_00501150_orig(c); return; }
    c->eax = (c->eax & ~0xffu) | 1u;
    POP32(c->eip); c->esp += 4u;                                          /* thiscall, ret 4 */
}

/* ---- the cover pictures: check, archive, list ---------------------------- */
static uint32_t T[256];
static uint32_t crc(const uint8_t *d, size_t n){
    if (!T[1]) for (uint32_t i = 0; i < 256; i++){ uint32_t x = i << 24; for (int b = 0; b < 8; b++) x = x & 0x80000000u ? (x << 1) ^ 0x04c11db7u : x << 1; T[i] = x; }
    uint32_t r = ~((uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3]);
    for (size_t i = 4; i < n; i++) r = ((r << 8) | d[i]) ^ T[r >> 24];
    return ~r;
}
static uint8_t g_pic[PIC_SIZE];
static uint32_t rd32le(const uint8_t *p){ return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
/* BF 0x712C2730: the whole file into g_pic, checked; 0 + why */
static int pic_read(const char *path, const char **why){
    FILE *f = fopen(path, "rb");
    if (!f){ *why = "cannot open"; return 0; }
    size_t n = fread(g_pic, 1, PIC_SIZE, f); int more = fgetc(f) != EOF; fclose(f);
    if (n != PIC_SIZE || more){ *why = "wrong size"; return 0; }
    if (rd32le(g_pic) != 0x4d433032u || rd32le(g_pic + 4) != PIC_SIZE || rd32le(g_pic + 8) != 8u || rd32le(g_pic + 12) != 0x100000u){
        *why = "not a cover picture (header)"; return 0; }
    if (crc(g_pic + 0x1c, 8) != rd32le(g_pic + 0x10) || crc(g_pic + PIC_DATA, 0x100000) != rd32le(g_pic + 0x14) ||
        crc(g_pic, 0x18) != rd32le(g_pic + 0x18)){ *why = "checksum mismatch"; return 0; }
    return 1;
}
static struct { char path[1100], label[48]; } V[MAXV];
static int g_nv, g_cur = -1, g_game = -1;            /* versions; the one shown; the game's own */
static char g_cover[32];                             /* "Magazine 3" */
static uint32_t g_screen, g_desc; static int g_loaded;
static int cover_dir(const char *profile, const char *cover, char *out, size_t n){
    char win[200];
    snprintf(win, sizeof win, "NFS Underground 2\\%s %s\\%s %s", profile, cover, profile, cover);
    if (!fileio_resolve_save(win, out, n)) return 0;
    char *s = strrchr(out, '/'); if (!s) return 0;
    *s = 0; return 1;                                /* the folder; the game's file is <folder>/<profile> <cover> */
}
/* the archive of profile's cover: "NFS Underground 2/<NAME>/BF/archive/<cover>/" (made if missing), so it goes with
 * the profile (Rename, backups, Delete: bf.c) and the game's cover folders keep only the game's own file */
int bf_path(const char *profile, const char *file, char *out, size_t n, int write);
static int archive_dir(const char *profile, const char *cover, char *out, size_t n){
    char f[120];
    snprintf(f, sizeof f, "archive\\%s\\x", cover);
    if (!bf_path(profile, f, out, n, 1)) return 0;
    char *s = strrchr(out, '/'); if (!s) return 0;
    *s = 0;                                          /* .../BF/archive/<cover> */
    s = strrchr(out, '/'); if (s){ *s = 0; mkdir(out, 0755); *s = '/'; }
    mkdir(out, 0755);
    return 1;
}
static int cmp_int(const void *a, const void *b){ return *(const int *)a - *(const int *)b; }
/* BF 0x712C2E50: the versions of profile's cover; the game's picture archived when it is new */
static void build_versions(const char *profile, const char *cover){
    char dir[1100], game[1200], p[1200]; const char *why = 0;
    int nums[MAXV], nn = 0;
    g_nv = 0; g_cur = g_game = -1;
    char gdir[1100];
    if (!cover_dir(profile, cover, gdir, sizeof gdir)){ fprintf(stderr, "[covers]   no picture of %s yet: nothing to browse\n", cover); return; }
    snprintf(game, sizeof game, "%s/%s %s", gdir, profile, cover);
    if (!archive_dir(profile, cover, dir, sizeof dir)){ fprintf(stderr, "[covers]   no archive folder for %s: nothing to browse\n", cover); return; }
    int valid = pic_read(game, &why);
    uint32_t c10 = rd32le(g_pic + 0x10), c14 = rd32le(g_pic + 0x14), c18 = rd32le(g_pic + 0x18);
    if (!valid) fprintf(stderr, "[covers]   the game's picture %s: %s (not archived)\n", game, why);
    DIR *d = opendir(dir);
    for (struct dirent *e; d && (e = readdir(d)); ){
        int v; char tail[8];
        if (sscanf(e->d_name, "%d.%7s", &v, tail) == 2 && !strcasecmp(tail, "COVER") && v > 0 && nn < MAXV) nums[nn++] = v;
    }
    if (d) closedir(d);
    qsort(nums, (size_t)nn, sizeof nums[0], cmp_int);
    for (int i = 0; i < nn && g_nv < MAXV; i++){
        snprintf(V[g_nv].path, sizeof V[0].path, "%s/%03d.cover", dir, nums[i]);
        snprintf(V[g_nv].label, sizeof V[0].label, "archive %03d", nums[i]);
        if (valid && g_game < 0){
            uint8_t h[PIC_DATA]; FILE *f = fopen(V[g_nv].path, "rb");
            if (f && fread(h, 1, sizeof h, f) == sizeof h && rd32le(h + 0x10) == c10 && rd32le(h + 0x14) == c14 && rd32le(h + 0x18) == c18) g_game = g_nv;
            if (f) fclose(f);
        }
        g_nv++;
    }
    if (valid && g_game < 0 && g_nv < MAXV){                  /* new: archived as the next number */
        int v = nn ? nums[nn - 1] + 1 : 1;
        snprintf(p, sizeof p, "%s/%03d.cover", dir, v);
        FILE *o = fopen(p, "wb");
        int w = o && fwrite(g_pic, 1, PIC_SIZE, o) == PIC_SIZE; if (o) w = fclose(o) == 0 && w;
        if (!w){ remove(p); fprintf(stderr, "[covers]   COULD NOT write %s\n", p); }
        else {
            snprintf(V[g_nv].path, sizeof V[0].path, "%s", p);
            snprintf(V[g_nv].label, sizeof V[0].label, "archive %03d", v);
            g_game = g_nv++;
            fprintf(stderr, "[covers]   archived the current picture as %s\n", p);
        }
    }
    if (s_all){                                               /* test: every cover of the save folder */
        char root[1100], *s;
        snprintf(root, sizeof root, "%s", gdir); s = strrchr(root, '/');     /* "NFS Underground 2" */
        if (s){ *s = 0; DIR *r = opendir(root); char pre[40]; snprintf(pre, sizeof pre, "%s ", profile);
            for (struct dirent *e; r && (e = readdir(r)) && g_nv < MAXV; ){
                const char *t = e->d_name;
                if (strncasecmp(t, pre, strlen(pre))) continue;
                t += strlen(pre);
                if (strncmp(t, "Magazine ", 9) && strncmp(t, "DVD ", 4)) continue;
                snprintf(V[g_nv].path, sizeof V[0].path, "%s/%s/%s", root, e->d_name, e->d_name);
                snprintf(V[g_nv].label, sizeof V[0].label, "test: %.40s", t);
                g_nv++;
            }
            if (r) closedir(r); }
    }
    fprintf(stderr, "[covers]   %d version(s) to browse%s; the game shows %s\n", g_nv, s_all ? " (incl. test pictures)" : "",
            g_game >= 0 ? V[g_game].label : "an unarchived picture");
}

/* ---- 3. photo taken: Showcase_MarkDone ------------------------------------ */
void sub_00533680_orig(CPU *c);
void sub_00533680(CPU *c){
    settings();
    if (s_on && post_career() && profile_ok()){
        int i = show_by(0x20u, MR32(G_SHOW_CUR)), k = i >= 0 ? show_entry(MR32(show_at((uint32_t)i) + 0x20u), 0) : -1;
        char nm[32]; cover_name(i, nm, sizeof nm);
        if (k >= 0 && S.waiting[k]){
            fprintf(stderr, "[covers] photo taken: %s; archiving the old picture first\n", nm);
            build_versions(S.profile, nm);
            S.waiting[k] = 0; S.shot[k] = ++S.seq;
            state_save();
            fprintf(stderr, "[covers]   %s counts as shot (#%d); the game now saves the new picture\n", nm, S.seq);
        } else if (i >= 0) fprintf(stderr, "[covers] photo taken: %s (not one of ours)\n", nm);
    }
    sub_00533680_orig(c);
}

/* ---- 4. photo pay --------------------------------------------------------- */
void sub_00541210_orig(CPU *c);
void sub_00541210(CPU *c){
    settings();
    uint32_t q = MR32(c->esp + 4u);
    if (!s_on || c->ecx != G_CAREER || !q || !ok(q, 0x40u) || !s_cash || !post_career()){ sub_00541210_orig(c); return; }
    float stars;
    {   /* 0x528410's st0, as career.c's guest_call_st0 reads it (the CPU, x87 stack too, restored after) */
        CPU save = *c;
        c->esp = (c->esp - 256u) & ~15u;
        c->ecx = G_CAREER;
        PUSH32(FN_RATING);
        dispatch_indirect(c, FN_RATING);
        stars = (float)c->st[c->top & 7];
        *c = save;
    }
    int32_t st = (int32_t)MR32(G_STAGE), per = 0;
    if (st >= 0 && st <= 7){ uint32_t info = MR32(G_STAGE_REC + (uint32_t)st * 0x104u + 0x100u); if (ok(info, 6u)) per = (int16_t)MR16(info + 4u); }
    int game_bonus = (int)((float)per * stars + 0.005f);
    int pay = (int)((float)s_cash * (stars < 1.f ? 1.f : stars) + 0.5f);
    int16_t base = (int16_t)MR16(q + 0x26u);
    int want = pay - game_bonus; want = want < -32768 ? -32768 : want > 32767 ? 32767 : want;
    uint32_t before = MR32(G_BANK);
    MR16(q + 0x26u) = (uint16_t)(int16_t)want;
    sub_00541210_orig(c);                       /* (pops its argument) */
    MR16(q + 0x26u) = (uint16_t)base;
    fprintf(stderr, "[covers] photo pay: %.2f stars x $%d = $%d (paid $%d; the game would have paid $%d)\n",
            (double)stars, s_cash, pay, (int)(MR32(G_BANK) - before), base + game_bonus);
}

/* ---- 5. the cover viewer -------------------------------------------------- */
static uint32_t g_scr;                          /* guest scratch for the counter's text */
static const char *word(int photo){
    static const struct { const char *lang, *photo, *updown; } W[] = {
        { "french", "PHOTO", "HAUT/BAS" }, { "german", "FOTO", "AUF/AB" }, { "italian", "FOTO", "SU/GI\xd9" },
        { "spanish", "FOTO", "ARRIBA/ABAJO" }, { "dutch", "FOTO", "OP/NEER" }, { "swedish", "FOTO", "UPP/NER" },
        { "danish", "FOTO", "OP/NED" },
    };
    const char *l = language_name();
    for (unsigned i = 0; i < sizeof W / sizeof W[0]; i++) if (!strcmp(l, W[i].lang)) return photo ? W[i].photo : W[i].updown;
    return photo ? "PHOTO" : "UP/DOWN";
}
static void counter(CPU *c, uint32_t screen){
    if (!s_counter || g_nv <= 1 || g_cur < 0 || !ok(screen, 8u)) return;
    uint32_t a[2] = { MR32(screen + 4u), s_counter == 2 ? 0xb8a7c6d0u : 0x47ff4e7cu };
    uint32_t obj = a[0] ? gcall(c, FN_FE_FIND, 0, 2, a) : 0;
    if (!ok(obj, 0x70u) || MR32(obj + 0x18u) != 2u) return;          /* a text object */
    if (!g_scr) g_scr = gheap_alloc(0, 128, 1);
    if (!g_scr) return;
    char t[128];
    if (s_counter == 2){
        uint32_t h = 0x001335f0u, b = gcall(c, FN_TEXT, 0, 1, &h);   /* "BACK" */
        char back[48] = ""; for (unsigned i = 0; b && i + 1 < sizeof back && MR8(b + i); i++){ back[i] = (char)MR8(b + i); back[i + 1] = 0; }
        snprintf(t, sizeof t, " ^ %s ^%s %d/%d  %s", back, word(1), g_cur + 1, g_nv, word(0));
    } else snprintf(t, sizeof t, "%s %d/%d  %s", word(1), g_cur + 1, g_nv, word(0));
    unsigned i = 0; for (; t[i] && i < 127; i++) MR8(g_scr + i) = (uint8_t)t[i];
    MR8(g_scr + i) = 0;
    gcall(c, FN_FE_SETSTR, obj, 1, &g_scr);
    MR32(obj + 0x1cu) |= 2u;
}
static uint32_t g_pad_was[8]; static int g_pad_primed;
static void browse(CPU *c, uint32_t screen, int dir){
    if (g_nv <= 1){ fprintf(stderr, "[covers]   %s pressed: %s has no other version yet\n", dir < 0 ? "UP" : "DOWN", g_cover); return; }
    int at = g_cur >= 0 ? g_cur : dir == -1 ? 0 : g_nv - 1;
    for (int tries = 0; tries < g_nv; tries++){
        at = (at + dir + g_nv) % g_nv;
        const char *why = 0;
        if (!pic_read(V[at].path, &why)){ fprintf(stderr, "[covers]   can't show %s: %s\n", V[at].label, why); continue; }
        uint32_t buf = MR32(G_SCREENCAP + 8u);
        if (!MR32(G_SCREENCAP + 0xcu) || !ok(buf, 0x100000u)){ fprintf(stderr, "[covers]   the viewer's picture buffer is gone\n"); return; }
        for (uint32_t i = 0; i < 0x100000u; i += 4) MR32(buf + i) = rd32le(g_pic + PIC_DATA + i);
        gcall(c, FN_UPLOAD, G_SCREENCAP, 0, 0);
        g_cur = at;
        fprintf(stderr, "[covers]   %s: showing %d/%d = %s%s\n", dir < 0 ? "UP" : "DOWN", at + 1, g_nv, V[at].label, at == g_game ? " (the game's current picture)" : "");
        counter(c, screen);
        return;
    }
}
void sub_00565b50_orig(CPU *c);
void sub_00565b50(CPU *c){
    settings();
    uint32_t screen = c->ecx, msg = MR32(c->esp + 4u);
    sub_00565b50_orig(c);                        /* (ret 0x10: the stack is popped) */
    if (!s_on) return;
    if (msg == MSG_OPEN){
        uint32_t desc = MR32(G_DESC);
        if (screen == g_screen && desc == g_desc && g_loaded) return;
        g_screen = screen; g_desc = desc; g_loaded = 0; g_pad_primed = 0; g_nv = 0; g_cur = g_game = -1;
        char nm[33]; live_profile(nm);
        if (!cover_name(show_by(0x28u, desc), g_cover, sizeof g_cover)){ fprintf(stderr, "[covers] viewer opened, but no cover has desc %08X\n", desc); return; }
        if (!nm[0] || strpbrk(nm, "\\/:*?\"<>|")){ fprintf(stderr, "[covers] viewer opened (%s), no usable profile name\n", g_cover); return; }
        fprintf(stderr, "[covers] viewer opened: %s of profile \"%s\"\n", g_cover, nm);
        build_versions(nm, g_cover);
    } else if (screen != g_screen) return;
    else if (msg == MSG_LOADED){ g_loaded = 1; g_cur = g_game; counter(c, screen); }
    else if (msg == MSG_FAILED || msg == MSG_BACK) g_loaded = 0;
    else if (msg == MSG_FRAME && g_loaded && MR8(screen + 0x50u) && MR8(screen + 0x51u)){
        uint32_t fe = MR32(G_FEMGR); if (!ok(fe, 0xd8u)) return;
        uint32_t pads = MR32(fe + 8u), n = MR32(fe + 0xd4u); if (n > 8) n = 8;
        if (!ok(pads, n * 0x88u + 8u)) return;
        int dir = 0;
        for (uint32_t i = 0; i < n; i++){
            uint32_t now = MR32(pads + i * 0x88u + 4u), down = now & ~g_pad_was[i];
            g_pad_was[i] = now;
            if (!g_pad_primed) continue;
            if (down & 1u) dir = (down & 2u) ? 1 : -1;     /* as BF: both -> +1 */
            else if (down & 2u) dir = 1;
        }
        g_pad_primed = 1;
        if (dir) browse(c, screen, dir);
    }
}

/* ---- 6. Endless Career's rebuild (BF export CoversAfterRebuild) ------------ */
void covers_after_rebuild(CPU *c){
    (void)c;
    settings();
    if (!s_on || !s_chal || MR32(G_CHAL_RUN) || !post_career() || !profile_ok()) return;
    uint32_t model = car_model(); if (!model) return;
    for (int k = 0; k < S.nshow; k++){
        if (S.waiting[k] != model) continue;
        int i = show_by(0x20u, S.show[k]);
        if (i < 0 || !has_challenge(show_at((uint32_t)i))) continue;
        uint32_t ev = challenge_of(S.show[k]); int slot = -1, there = 0;
        for (int j = 0; j < 32; j++){ uint32_t v = MR32(G_CHAL_OPEN + 4u * (uint32_t)j); if (v == ev) there = 1; else if (!v && slot < 0) slot = j; }
        if (there) continue;
        if (slot < 0){ fprintf(stderr, "[covers] the world challenge list is full; a magazine challenge was not added\n"); return; }
        MR32(G_CHAL_OPEN + 4u * (uint32_t)slot) = ev;
    }
}
