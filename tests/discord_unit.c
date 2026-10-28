/* discord_unit.c -- tests for runtime/discord.c without the game.
 *
 *   tests/discord_unit.sh            build and run every case (with the mock)
 *   out/discord_unit <case> [mock.py tmpdir]
 *
 * Cases:
 *   text      JSON escaping, Windows-1252 -> UTF-8, clamping to 128 characters
 *   state     situation -> text on a synthetic guest memory image (game flow,
 *             race parameters and status, string/track/car tables, screens)
 *   ipc       against tools/mock_discord.py: handshake, READY, a session of
 *             presence changes incl. a burst (rate limit, coalescing), odd
 *             characters, PING/PONG, clear + CLOSE on quit
 *   bench     cost of the Present hook per frame (off, and on)
 *   absent    no Discord: no connection, ~zero CPU, quick quit
 *   late      Discord appears after the game started -> connects
 *   restart   Discord killed and started again -> reconnects, shows it again
 *   guard     which runs count as tests (no discovery without the mock), and
 *             that discovery stays inside a test's private $TMPDIR (and /tmp)
 *   park      the app parks the game in the background (no Present, no
 *             samples): a race shows "Paused" until the game samples again
 *   noready close garbage slow error badid noread
 *             a misbehaving Discord: no hang, no crash, bounded quit (noread:
 *             it stops reading, so the client's writes block)
 * Every case prints "PASS <case> ..." or "FAIL <case> ..." lines; IPC cases
 * also leave <tmpdir>/<case>.jsonl (the mock's frame log) for
 * tests/discord_check.py.  Timings are shortened here (DS_* overrides).
 */
#define DS_SAMPLE_HZ   50
#define DS_SETTLE_MS   150
#define DS_RATE_N      4
#define DS_RATE_WIN    3.0
#define DS_MINOR_GAP   1.5
#define DS_READY_TMO   1.0
#define DS_RETRY_MIN   0.2
#define DS_RETRY_MAX   0.8
#define DS_UNPARK_S    2.0
static int g_test_parked;                      /* lifecycle_game_parked() of the app */
#define DS_PARKED()    g_test_parked
#include "../runtime/discord.c"

#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>

extern char **environ;
static int g_fail;
#define CHECK(cond, ...) do{ if (cond) printf("PASS " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); g_fail = 1; } \
                             printf("\n"); fflush(stdout); }while(0)

/* ---- stubs for autotest.c ------------------------------------------- */
static const char *g_top, *g_scr;
static const char *g_batch_t[8];               /* screens initialised in the frame of g_scr */
const char *autotest_top_fng(void){ return g_top; }
const char *autotest_screen(void){ return g_scr; }
const char *autotest_screen_batch(int i){
    if (!g_batch_t[0]) return i == 0 ? g_scr : 0;           /* default: g_scr alone */
    return i >= 0 && i < 8 ? g_batch_t[i] : 0;
}
/* ---- stub for roam_game.c: free-roam friends connected ---------------- */
static int g_test_friends;
int roam_game_friends(void){ return g_test_friends; }
/* ---- stub for language.c: the game's "No Profile" (English here) -------- */
int language_no_profile(const char *name){ return !strcasecmp(name, "No Profile"); }
/* ---- stub for winsock.c: a LAN session is open ------------------------ */
static int g_test_lan;
int winsock_session_active(void){ return g_test_lan; }
/* ---- stub for garage.c: a car record by handle (the table's; no slots 6-10 here) */
uint32_t garage_find(uint32_t db, uint32_t h){
    for (uint32_t i = 0; h && i < 0x5bu; i++){ uint32_t r = MR32(db + 0x9bd8u + i * 4u); if (r && MR32(r + 8u) == h) return r; }
    return 0;
}

/* ---- synthetic guest memory ----------------------------------------- */
#define HEAP 0x10000000u
static uint32_t g_hp = HEAP;
static uint32_t galloc(uint32_t n){ uint32_t a = g_hp; g_hp += (n + 15u) & ~15u; return a; }
static uint32_t gputs(const char *s){ uint32_t a = galloc((uint32_t)strlen(s) + 1); memcpy(g_mem + a, s, strlen(s) + 1); return a; }
static void W32(uint32_t a, uint32_t v){ MR32(a) = v; }
static void W8(uint32_t a, uint8_t v){ MR8(a) = v; }
static void WF(uint32_t a, float f){ memcpy(g_mem + a, &f, 4); }

typedef struct { const char *key, *text; } Str;
static int cmp_hash(const void *a, const void *b){
    uint32_t x = bhash(((const Str *)a)->key), y = bhash(((const Str *)b)->key);
    return x < y ? -1 : x > y;
}
static uint32_t g_rs, g_player, g_car;
static void mem_setup(void){
    mem_init();
    mem_commit(0x00820000u, 0x00090000u);       /* the game's .data/.bss globals we read */
    mem_commit(HEAP, 0x00100000u);
    /* localised strings (sorted by hash, as the game keeps them) */
    static Str strs[] = {
        { "TRACK_NAME_4014", "Resort Loop" }, { "REVERSE_TRACK_NAME_4014", "Resort Loop Reverse" },
        { "TRACK_NAME_4123", "Mall Center" }, { "TRACK_NAME_4201", "Bayview Bridge" },
        { "TRACK_NAME_4312", "Stadium Drift 2" }, { "TRACK_NAME_4603", "Industrial Park" },
        { "TRACK_NAME_4701", "Speedway" }, { "TRACK_NAME_4000", "4000" },
        { "CAR_NAME_PEUGOT", "206" }, { "CAR_NAME_350Z", "350Z" },
        { "TRACK_NAME_4999", "Caf\xE9 \x93Quote\x94 \\ Back\x80" },     /* cp1252: e-acute, curly quotes, euro */
    };
    int n = (int)(sizeof strs / sizeof *strs);
    qsort(strs, (size_t)n, sizeof *strs, cmp_hash);
    uint32_t tab = galloc((uint32_t)n * 8u);
    for (int i = 0; i < n; i++){ W32(tab + (uint32_t)i * 8u, bhash(strs[i].key)); W32(tab + (uint32_t)i * 8u + 4u, gputs(strs[i].text)); }
    W32(G_LANG_N, (uint32_t)n); W32(G_LANG_TAB, tab);
    /* tracks */
    static const int16_t ids[] = { 4000, 4014, 4123, 4201, 4312, 4603, 4701, 4999 };
    uint32_t tr = galloc(8u * 0x128u);
    for (int i = 0; i < 8; i++) MR16(tr + (uint32_t)i * 0x128u + 0x8au) = (uint16_t)ids[i];
    W32(G_TRACKS, tr); W32(G_TRACKS_N, 8);
    /* car types: 0 = PEUGOT/PEUGEOT, 1 = 350Z/NISSAN */
    uint32_t ct = galloc(2u * 0x890u);
    memcpy(g_mem + ct, "PEUGOT", 7); memcpy(g_mem + ct + 0xc0u, "PEUGEOT", 8);
    memcpy(g_mem + ct + 0x890u, "350Z", 5); memcpy(g_mem + ct + 0x890u + 0xc0u, "NISSAN", 7);
    W32(G_CARTYPES, ct);
    /* racers (player 1 = racer 0, car type 0) */
    uint32_t racers = galloc(4u * 0x940u);
    W32(G_RACE + R_RACERS, racers); W32(G_RACE + R_NRACERS, 4); W32(G_RACE + R_HUMAN, 0);
    W32(racers + 0x10u, 0);
    /* player object and race status: 4 entries, player 1 = entry 2 */
    g_car = galloc(64); g_player = galloc(64); W32(g_player + 4, g_car); W32(G_PLAYER1, g_player);
    g_rs = galloc(RS_ENTRY + 6u * RS_ENTRY_SZ);
    for (uint32_t i = 0; i < 4; i++){
        uint32_t e = g_rs + RS_ENTRY + i * RS_ENTRY_SZ;
        W32(e, i == 2 ? g_car : galloc(16)); W8(e + 0xa, (uint8_t)(i + 1));
    }
    W32(g_rs + RS_COUNT, 4);
    /* entries 4 and 5 are never written by the game: they keep what the
     * memory held, here the 0xEE fill of the game's own pool */
    memset(g_mem + g_rs + RS_ENTRY + 4u * RS_ENTRY_SZ, 0xEE, 2u * RS_ENTRY_SZ);
    W32(G_RSTATUS, g_rs);
    /* front-end car record for the quick-race handle */
    /* a stock record (class 0x79aadc, type at +0x18) and a customized one
     * (class 0x79b12c, type name hash at +0x20) */
    uint32_t rec = galloc(64); W32(rec, 0x0079aadcu); W32(rec + 8, 0xa8ecfbe4u); W32(rec + 0x18, 1);
    W32(G_FE_CARS + 0x9bd8u + 5u * 4u, rec); W32(G_FE_CAR_QR, 0xa8ecfbe4u);
    uint32_t rec2 = galloc(64); W32(rec2, 0x0079b12cu); W32(rec2 + 8, 0x6996c983u); W32(rec2 + 0x20, bhash("PEUGOT"));
    W32(G_FE_CARS + 0x9bd8u + 9u * 4u, rec2);
    W32(ct + 0xd0u, bhash("PEUGOT")); W32(ct + 0x890u + 0xd0u, bhash("350Z"));
    memcpy(g_mem + G_PROFILE, "CLAUDE", 7);
}
static uint32_t entry(int i){ return g_rs + RS_ENTRY + (uint32_t)i * RS_ENTRY_SZ; }
static void set_race(uint32_t track, uint32_t kind, uint32_t laps, int p2p, int drag, int drift, int url, int sx){
    W32(G_RACE + R_TRACK, track); W32(G_RACE + R_KIND, kind); W32(G_RACE + R_LAPS, laps);
    W32(G_RACE + R_P2P, (uint32_t)p2p); W8(G_RACE + R_DRAG, (uint8_t)drag); W8(G_RACE + R_DRIFT, (uint8_t)drift);
    W8(G_RACE + R_URL, (uint8_t)url); W8(G_RACE + R_SHORT, (uint8_t)sx); W32(G_RACE + R_REVERSE, 0);
}
/* game situations used by the tests */
static void sit_boot(void){ W32(G_FLOW, 0); g_top = 0; g_scr = 0; }
static void sit_menus(void){ W32(G_FLOW, 3); W32(G_FE_MODE, 2); g_top = "GarageMain.fng"; g_scr = "UI_Main.fng"; }
static void sit_custom(void){ W32(G_FLOW, 3); W32(G_FE_MODE, 32); g_top = "GarageMain.fng"; g_scr = "UI_GenericParts_Browser.fng"; }
static void sit_loading(void){ W32(G_FLOW, 4); W32(G_FE_MODE, 2); set_race(4014, 1, 3, 0, 0, 0, 0, 0); g_top = "PC_Loading.fng"; g_scr = "PC_Loading.fng"; }
static void sit_race(int lap_done, int pos){
    W32(G_FLOW, 6); W32(G_FE_MODE, 2); set_race(4014, 1, 3, 0, 0, 0, 0, 0);
    g_top = "HUD_SingleRace.fng"; g_scr = "HUD_SingleRace.fng";
    W32(g_rs, 4); W32(g_rs + 0x14, 4000u * 30u);          /* racing, clock at 30 s */
    for (int i = 0; i < 4; i++){ W8(entry(i) + 9, (uint8_t)lap_done); W8(entry(i) + 0xa, (uint8_t)(i + 1)); }
    W8(entry(2) + 0xa, (uint8_t)pos);                     /* player 1's entry */
}
static void sit_roam(void){
    W32(G_FLOW, 6); W32(G_FE_MODE, 1); set_race(4000, 0, 0, 0, 0, 0, 0, 0); W32(G_STAGE, 1);
    g_top = "HUD_SingleRace.fng"; g_scr = "Chyron_IG.fng";
    W32(G_NIS_ACTIVE2, 0);
}

static void sample(Presence *p){ memset(p, 0, sizeof *p); ds_sample(p); utf8_clamp(p->details, sizeof p->details, DS_MAXCHARS); utf8_clamp(p->state, sizeof p->state, DS_MAXCHARS); }
static void expect(const char *what, const char *d, const char *s){
    Presence p; sample(&p);
    CHECK(!strcmp(p.details, d) && !strcmp(p.state, s), "state %-28s -> \"%s\" | \"%s\"%s%s%s%s", what, p.details, p.state,
          strcmp(p.details, d) || strcmp(p.state, s) ? "   (want \"" : "", strcmp(p.details, d) || strcmp(p.state, s) ? d : "",
          strcmp(p.details, d) || strcmp(p.state, s) ? "\" | \"" : "", strcmp(p.details, d) || strcmp(p.state, s) ? s : "");
}

/* ---- case: text ------------------------------------------------------ */
static void case_text(void){
    char out[256];
    json_str(out, sizeof out, "a\"b\\c\n\x01\x1f" "\xC3\xA9");
    CHECK(!strcmp(out, "\"a\\\"b\\\\c\\u000a\\u0001\\u001f\xC3\xA9\""), "text json escaping: %s", out);
    const uint8_t g[] = { 'C','a','f',0xE9,' ',0x93,'x',0x94,' ',0x80,'\t','\n','e','n','d',' ',' ',0 };
    guest_to_utf8(out, sizeof out, g, sizeof g);
    CHECK(!strcmp(out, "Caf\xC3\xA9 \xE2\x80\x9Cx\xE2\x80\x9D \xE2\x82\xAC end"), "text cp1252 -> utf8: %s", out);
    char big[DS_TXT]; size_t o = 0;
    for (int i = 0; i < 200 && o + 3 < sizeof big; i++){ big[o++] = (char)0xC3; big[o++] = (char)0xA9; }
    big[o] = 0;
    utf8_clamp(big, sizeof big, DS_MAXCHARS);
    int chars = 0; for (const unsigned char *q = (const unsigned char *)big; *q; q++) if ((*q & 0xC0) != 0x80) chars++;
    CHECK(chars == 128 && !strcmp(big + strlen(big) - 3, "\xE2\x80\xA6"), "text clamp to %d characters with an ellipsis", chars);
    char bad[16] = { 'a', (char)0xFF, (char)0xC3, 'b', 0 };
    utf8_clamp(bad, sizeof bad, DS_MAXCHARS);
    CHECK(!strcmp(bad, "a\xEF\xBF\xBD\xEF\xBF\xBD" "b"), "text invalid UTF-8 -> U+FFFD");
    /* 4-byte forms: beyond U+10FFFF (F4 90.., F5..F7) and overlong (F0 80..) are invalid */
    static const char *const bad4[] = { "\xF5\x80\x80\x80", "\xF7\xBF\xBF\xBF", "\xF4\x90\x80\x80", "\xF0\x80\x80\x80", 0 };
    int ok4 = 1;
    for (int i = 0; bad4[i]; i++){
        char b[16]; snprintf(b, sizeof b, "a%sb", bad4[i]); utf8_clamp(b, sizeof b, DS_MAXCHARS);
        if (strcmp(b, "a\xEF\xBF\xBD" "b")) ok4 = 0;
    }
    char emo[16] = "a\xF0\x9F\x98\x80" "b"; utf8_clamp(emo, sizeof emo, DS_MAXCHARS);
    CHECK(ok4 && !strcmp(emo, "a\xF0\x9F\x98\x80" "b"), "text invalid 4-byte UTF-8 -> U+FFFD, a valid one is kept");
    /* random bytes: the clamped text is always strict UTF-8 of <= 128 UTF-16 units */
    unsigned rng = 1; int fuzz_bad = 0;
    for (int it = 0; it < 50000; it++){
        char s[DS_TXT]; int len = (int)((rng = rng * 1103515245u + 12345u) >> 8) % (DS_TXT - 1);
        for (int i = 0; i < len; i++){ rng = rng * 1103515245u + 12345u; s[i] = (char)(0x80 | (rng >> 16)); if (!(rng & 0x700)) s[i] = (char)(0x20 + (rng >> 24) % 0x5f); }
        s[len] = 0;
        utf8_clamp(s, sizeof s, DS_MAXCHARS);
        int units = 0;
        for (const unsigned char *q = (const unsigned char *)s; *q; ){
            unsigned c = *q, k = c < 0x80 ? 0 : c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : 9;
            if (k == 9){ fuzz_bad++; break; }
            unsigned cp = k ? c & (0x7Fu >> (k + 1)) : c;
            for (unsigned j = 1; j <= k; j++){ if ((q[j] & 0xC0) != 0x80){ k = 9; break; } cp = cp << 6 | (q[j] & 0x3F); }
            if (k == 9 || (k == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (k == 3 && (cp < 0x10000 || cp > 0x10FFFF))){ fuzz_bad++; break; }
            units += k == 3 ? 2 : 1; q += k + 1;
        }
        if (units > DS_MAXCHARS) fuzz_bad++;
    }
    CHECK(fuzz_bad == 0, "text 50000 random byte strings -> strict UTF-8, <= %d UTF-16 units (%d bad)", DS_MAXCHARS, fuzz_bad);
    char t[32]; thousands(12345, t, sizeof t); CHECK(!strcmp(t, "12,345"), "text thousands %s", t);
    thousands(999, t, sizeof t); CHECK(!strcmp(t, "999"), "text thousands %s", t);
    thousands(1234567, t, sizeof t); CHECK(!strcmp(t, "1,234,567"), "text thousands %s", t);
    CHECK(!strcmp(ordinal(1), "1st") && !strcmp(ordinal(2), "2nd") && !strcmp(ordinal(3), "3rd") &&
          !strcmp(ordinal(4), "4th") && !strcmp(ordinal(11), "11th") && !strcmp(ordinal(22), "22nd"), "text ordinals");
}

/* ---- case: state ----------------------------------------------------- */
static void case_state(void){
    mem_setup();
    g_session_start = 1792000000;
    sit_boot();                                         expect("boot", "Starting up", "");
    W32(G_FLOW, 3); g_top = "UG_LS_Splash.fng"; g_scr = "Chyron_FE.fng";
                                                        expect("title screen", "At the title screen", "");
    sit_menus();                                        expect("main menu", "In the menus", "");
    g_scr = "GenericDialog_SMALL.fng";                  expect("dialog over the menu", "In the menus", "");
    sit_custom();                                       expect("customize (parts)", "Customizing", "Nissan 350Z");
    W32(G_FE_CAR_QR, 0x6996c983u);                      expect("customize a customized car", "Customizing", "Peugeot 206");
    W32(G_FE_CAR_QR, 0x12345678u);                      expect("customize, unknown handle", "Customizing", "");
    W32(G_FE_CAR_QR, 0xa8ecfbe4u);
    sit_loading();                                      expect("loading a race", "Loading\xE2\x80\xA6", "Circuit \xE2\x80\x94 Resort Loop");
    sit_race(0, 2); W32(g_rs, 3); W32(g_rs + 0x14, 0);  expect("countdown", "Circuit \xE2\x80\x94 Resort Loop", "On the starting line");
    sit_race(1, 2);                                     expect("circuit lap 2", "Circuit \xE2\x80\x94 Resort Loop", "Lap 2/3 \xC2\xB7 2nd of 4");
    Presence p; sample(&p);
    CHECK(p.start == (int64_t)time(0) - 30 || p.start == (int64_t)time(0) - 29, "state race timer anchored on the race clock (start %lld)", (long long)p.start);
    W32(G_RACE + R_REVERSE, 1);                         expect("reversed track", "Circuit \xE2\x80\x94 Resort Loop Reverse", "Lap 2/3 \xC2\xB7 2nd of 4");
    W32(G_RACE + R_REVERSE, 0);
    g_top = "UI_pause.fng";                             expect("paused", "Circuit \xE2\x80\x94 Resort Loop", "Paused");
    sit_race(3, 1);                                     expect("finished", "Circuit \xE2\x80\x94 Resort Loop", "Finished 1st of 4");
    { Presence a, b; sample(&a); W32(g_rs + 0x14, 4000u * 5u);   /* the clock stops or jumps after the finish */
      sample(&b); CHECK(a.start == b.start, "state finished: the timer stays put (%lld %lld)", (long long)a.start, (long long)b.start); }
    { Presence a, l, b; sample(&a);                     /* the game shows PC_Loading for a few frames at the race end */
      g_top = "PC_Loading.fng"; sample(&l); g_top = "UI_PostRaceResults.fng"; sample(&b);
      CHECK(l.phase == PH_LOADING && b.phase == PH_FINISHED && a.start == b.start,
            "state results after the end-of-race PC_Loading keep the race timer (%lld %lld)", (long long)a.start, (long long)b.start); }
    sit_race(0, 3); set_race(4123, 1, 1, 1, 0, 0, 0, 0); expect("sprint", "Sprint \xE2\x80\x94 Mall Center", "3rd of 4");
    set_race(4201, 1, 1, 1, 1, 0, 0, 0);                expect("drag", "Drag \xE2\x80\x94 Bayview Bridge", "3rd of 4");
    set_race(4312, 1, 3, 0, 0, 1, 0, 0); WF(entry(2) + 0x140, 12345.6f);      /* lap 1 in progress */
                                                        expect("drift", "Drift \xE2\x80\x94 Stadium Drift 2", "Lap 1/3 \xC2\xB7 1st of 4 \xC2\xB7 12,345 pts");
    WF(entry(0) + 0x140, 20000.0f);                     expect("drift, ranked by points", "Drift \xE2\x80\x94 Stadium Drift 2", "Lap 1/3 \xC2\xB7 2nd of 4 \xC2\xB7 12,345 pts");
    WF(entry(0) + 0x140, 0.0f);
    W8(entry(2) + 9, 1); WF(entry(2) + 0x144, 1000.4f); WF(entry(2) + 0x190, 12345.6f);
                                                        expect("drift lap 2 (finished laps + current)", "Drift \xE2\x80\x94 Stadium Drift 2", "Lap 2/3 \xC2\xB7 1st of 4 \xC2\xB7 13,346 pts");
    W8(entry(2) + 9, 0);
    set_race(4603, 1, 10, 0, 0, 0, 0, 1);               expect("street x", "Street X \xE2\x80\x94 Industrial Park", "Lap 1/10 \xC2\xB7 3rd of 4");
    set_race(4701, 1, 3, 0, 0, 0, 1, 0);                expect("url", "URL \xE2\x80\x94 Speedway", "Lap 1/3 \xC2\xB7 3rd of 4");
    set_race(4014, 3, 3, 0, 0, 0, 0, 0);                expect("lap knockout", "Lap Knockout \xE2\x80\x94 Resort Loop", "Lap 1/3 \xC2\xB7 3rd of 4");
    set_race(4999, 1, 3, 0, 0, 0, 0, 0);                expect("odd track name (cp1252)",
        "Circuit \xE2\x80\x94 Caf\xC3\xA9 \xE2\x80\x9CQuote\xE2\x80\x9D \\ Back\xE2\x82\xAC", "Lap 1/3 \xC2\xB7 3rd of 4");
    W32(G_FE_MODE, 1); set_race(4014, 1, 3, 0, 0, 0, 0, 0);
                                                        expect("career race", "Career Circuit \xE2\x80\x94 Resort Loop", "Lap 1/3 \xC2\xB7 3rd of 4");
    sit_roam();                                         expect("career free roam", "Career \xE2\x80\x94 Stage 1", "Free roaming as CLAUDE");
    W32(G_STAGE, 0);                                    expect("career prologue (stage 0)", "Career \xE2\x80\x94 Prologue", "Free roaming as CLAUDE");
    W32(G_STAGE, 5);                                    expect("career stage 5", "Career \xE2\x80\x94 Stage 5", "Free roaming as CLAUDE");
    W32(G_STAGE, 6);                                    expect("career complete (stage 6)", "Career \xE2\x80\x94 Complete", "Free roaming as CLAUDE");
    W32(G_STAGE, 1);
    memcpy(g_mem + G_PROFILE, "A\"B\\\x01\xE9", 7);    expect("odd profile name", "Career \xE2\x80\x94 Stage 1", "Free roaming as A\"B\\ \xC3\xA9");
    memcpy(g_mem + G_PROFILE, "CLAUDE", 7);
    W32(G_NIS_ACTIVE2, 1);                              expect("in-engine cutscene", "Watching a cutscene", "");
    W32(G_NIS_ACTIVE2, 0);
    g_top = "UI_Pause.fng";                             expect("career paused", "Career \xE2\x80\x94 Stage 1", "Paused");
    /* free-roam multiplayer: friends connected */
    g_test_friends = 2;                                 expect("paused, 2 friends connected", "Career \xE2\x80\x94 Stage 1", "Paused");
    g_top = "HUD_SingleRace.fng";                       expect("free roam, 2 friends", "Career \xE2\x80\x94 Stage 1", "Free roam with 2 friends");
    g_test_friends = 1;                                 expect("free roam, 1 friend", "Career \xE2\x80\x94 Stage 1", "Free roam with 1 friend");
    g_minimal = 1;                                      expect("free roam, friend, minimal", "", "");
    g_minimal = 0; W32(G_FE_MODE, 2); set_race(4000, 0, 0, 0, 0, 0, 0, 0);
                                                        expect("free run (not career), friend", "Free Run", "Peugeot 206");
    W32(G_FE_MODE, 1); g_test_friends = 0;              expect("free roam, friends gone", "Career \xE2\x80\x94 Stage 1", "Free roaming as CLAUDE");
    g_top = "UI_Pause.fng";
    /* the car lot: UI_CareerCarLot, GarageMain (backdrop) and Chyron_FE come in one frame */
    W32(G_FLOW, 3); W32(G_STAGE, 0); g_top = "GarageMain.fng"; g_scr = "Chyron_FE.fng";
    g_batch_t[0] = "UI_CareerCarLot.fng"; g_batch_t[1] = "GarageMain.fng"; g_batch_t[2] = "Chyron_FE.fng";
                                                        expect("career car lot", "Choosing a car", "");
    g_batch_t[0] = "UI_Main.fng"; g_batch_t[1] = "GarageMain.fng"; g_batch_t[2] = 0; g_scr = "GarageMain.fng";
                                                        expect("career menus", "In the menus", "Career \xE2\x80\x94 Prologue");
    g_batch_t[0] = 0; W32(G_STAGE, 1); W32(G_FLOW, 6);
    W32(G_FE_MODE, 2); set_race(4000, 0, 0, 0, 0, 0, 0, 0); g_top = "HUD_SingleRace.fng";
                                                        expect("free run", "Free Run", "Peugeot 206");
    W32(G_FLOW, 3); W32(G_FE_MODE, 1); g_top = "PC_Loading.fng"; g_scr = "IG_PlayMovie.fng";
                                                        expect("story movie", "Watching a cutscene", "");
    W32(G_FLOW, 4);                                     expect("movie skipped, loading", "Loading\xE2\x80\xA6", "Career");
    /* LAN play: the LAN screens while a LAN session is open, a network race */
    g_test_lan = 1; W32(G_FLOW, 3); W32(G_FE_MODE, 2);
    g_top = g_scr = "UI_PC_LAN_ServerSelect.fng";       expect("LAN server list", "LAN lobby", "Looking for games");
    g_top = g_scr = "UI_OLMAIN.fng";                    expect("LAN main menu", "LAN lobby", "");
    g_top = g_scr = "UI_OLCarSelect.fng";               expect("LAN car choice", "LAN lobby", "Choosing a car");
    g_top = g_scr = "PC_OL_Lobby.fng";                  expect("LAN lobby", "LAN lobby", "");
    g_top = g_scr = "UI_QRTrackSelect.fng";             expect("LAN create game", "LAN lobby", "Creating a game");
    g_top = g_scr = "PC_OL_GameRoom.fng";               expect("LAN game room", "LAN lobby", "In a game room");
    g_scr = "GenericDialog_SMALL.fng";                  expect("dialog over the game room", "LAN lobby", "In a game room");
    W32(G_FLOW, 4); set_race(4014, 1, 2, 0, 0, 0, 0, 0); g_top = g_scr = "PC_Loading.fng";
                                                        expect("LAN race loading", "Loading\xE2\x80\xA6", "LAN race \xE2\x80\x94 Circuit \xE2\x80\x94 Resort Loop");
    sit_race(0, 2); W32(G_RACE + R_LAPS, 2); W32(G_NET_CLIENT, 1);
                                                        expect("LAN race", "LAN race \xE2\x80\x94 Circuit \xE2\x80\x94 Resort Loop", "Lap 1/2 \xC2\xB7 2nd of 4");
    g_top = "UI_pause.fng";                             expect("LAN race pause menu (not paused)", "LAN race \xE2\x80\x94 Circuit \xE2\x80\x94 Resort Loop", "Lap 1/2 \xC2\xB7 2nd of 4");
    W32(G_NET_CLIENT, 0); W32(G_NET_HOST, 1); set_race(4123, 1, 1, 1, 0, 0, 0, 0); g_top = "HUD_SingleRace.fng";
                                                        expect("LAN sprint, host", "LAN race \xE2\x80\x94 Sprint \xE2\x80\x94 Mall Center", "2nd of 4");
    sit_race(1, 1); set_race(4014, 1, 1, 0, 0, 0, 0, 0); W32(G_NET_HOST, 1);
                                                        expect("LAN race finished", "LAN race \xE2\x80\x94 Circuit \xE2\x80\x94 Resort Loop", "Finished 1st of 4");
    g_minimal = 1;                                      expect("LAN race, minimal", "", "");
    g_minimal = 0; W32(G_NET_HOST, 0); W8(G_NET_ONLINE, 1); sit_race(0, 3);
                                                        expect("LAN race (World online flag)", "LAN race \xE2\x80\x94 Circuit \xE2\x80\x94 Resort Loop", "Lap 1/3 \xC2\xB7 3rd of 4");
    W8(G_NET_ONLINE, 0); W32(G_FLOW, 3); g_top = "GarageMain.fng"; g_scr = "UI_Main.fng";
                                                        expect("main menu, LAN session still open", "LAN lobby", "");
    g_test_lan = 0;                                     expect("main menu after the LAN session", "In the menus", "");
    /* EA Online shares the UI_OL* screens; they are LAN screens only on the LAN branch */
    g_top = g_scr = "UI_OLISPConnect.fng";              expect("EA Online connect (not LAN)", "In the menus", "");
    g_test_lan = 1;                                     /* the Online item may start Winsock too */
    g_top = g_scr = "UI_OLEALogin.fng";                 expect("EA Online login, Winsock up (not LAN)", "In the menus", "");
    g_test_lan = 0; g_top = "GarageMain.fng"; g_scr = "UI_Main.fng";
                                                        expect("main menu after EA Online", "In the menus", "");
    g_minimal = 1; sit_race(1, 2);                      expect("minimal detail", "", "");
    sample(&p); CHECK(p.active && p.start == g_session_start, "state minimal keeps the session timer");
    g_minimal = 0;
    /* robustness: tables missing or garbage must not crash and must not invent text */
    W32(G_LANG_N, 0x7fffffffu); sit_race(1, 2);         expect("no string table", "Circuit", "Lap 2/3 \xC2\xB7 2nd of 4");
    W32(G_RSTATUS, 0x7ff00000u);                        expect("race status unmapped", "Circuit", "");
}

/* ---- the mock -------------------------------------------------------- */
static pid_t g_mock = -1;
static char g_sock[200], g_log_path[300];
static void mock_start(const char *py, const char *tmp, const char *name, const char *mode, int append){
    snprintf(g_sock, sizeof g_sock, "%s/%s.sock", tmp, name);
    snprintf(g_log_path, sizeof g_log_path, "%s/%s.jsonl", tmp, name);
    if (!append) unlink(g_log_path);
    char *argv[] = { "/usr/bin/python3", "-I", (char *)py, g_sock, "--log", g_log_path, "--mode", (char *)mode,
                     "--ping", "0.4", "--rate-n", "5", "--rate-window", "3", "--reply-delay", "3", 0 };
    /* the mock checks Discord's rule scaled to the test: 5 per DS_RATE_WIN; the
     * client keeps to DS_RATE_N (4) per window plus the one clear at quit */
    if (posix_spawn(&g_mock, argv[0], 0, 0, argv, environ) != 0){ printf("FAIL cannot start the mock\n"); exit(1); }
    for (int i = 0; i < 200; i++){ struct stat st; if (stat(g_sock, &st) == 0) return; usleep(10000); }
    printf("FAIL mock socket did not appear\n"); exit(1);
}
static void mock_stop(int sig){
    if (g_mock > 0){ kill(g_mock, sig); int st; waitpid(g_mock, &st, 0); g_mock = -1; }
}
/* Drive the game-thread hook for `sec` seconds at ~120 fps; returns the
 * longest single discord_on_present call in microseconds. */
/* CPU time of the calling thread: what a call costs the game thread, without
 * the preemption a loaded machine adds to wall time (reported separately). */
static double thread_cpu_us(void){
    struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}
static double g_max_call_us, g_max_wall_us, g_init_us = -1;
static void run_frames(double sec){
    double end = mono_s() + sec;
    while (mono_s() < end){
        double a = mono_s(), ca = thread_cpu_us();
        discord_on_present();
        double us = thread_cpu_us() - ca, wus = (mono_s() - a) * 1e6;
        if (g_init_us < 0){ g_init_us = us; }               /* the first call starts the IPC thread */
        else { if (us > g_max_call_us) g_max_call_us = us; if (wus > g_max_wall_us) g_max_wall_us = wus; }
        usleep(8333);
    }
}
static void ipc_env(int log){
    setenv("NFSU2_DISCORD", "1", 1);
    setenv("NFSU2_DISCORD_IPC", g_sock, 1);
    char l[8]; snprintf(l, sizeof l, "%d", log); setenv("NFSU2_DISCORD_LOG", l, 1);
    unsetenv("NFSU2_INPUT_SCRIPT"); unsetenv("NFSU2_QUIT_FRAMES");
}
static double timed_shutdown(void){ double a = mono_s(); discord_shutdown(); return (mono_s() - a) * 1e3; }

int main(int argc, char **argv){
    setvbuf(stdout, 0, _IOLBF, 0);
    const char *c = argc > 1 ? argv[1] : "text";
    const char *py = argc > 2 ? argv[2] : "tools/mock_discord.py";
    const char *tmp = argc > 3 ? argv[3] : "/tmp";
    if (!strcmp(c, "text")){ case_text(); return g_fail; }
    if (!strcmp(c, "state")){ case_state(); return g_fail; }
    if (!strcmp(c, "guard")){
        /* which runs count as tests (never look for the Discord app); pure
         * function, nothing connects */
        static const char *const vars[] = { "NFSU2_INPUT_SCRIPT", "NFSU2_QUIT_FRAMES", "QUITFRAMES", "WINDOW", "NOFOCUS", "ALARM", 0 };
        struct { const char *env[6]; int automated; const char *what; } t[] = {
            { { 0, 0, 0, "1", 0, "0" }, 0, "run.sh / the app (WINDOW=1 ALARM=0)" },
            { { 0, 0, 0, "1", "0", "0" }, 0, "NOFOCUS=0" },
            { { "s.txt", 0, 0, "1", 0, "0" }, 1, "input script" },
            { { 0, "100", 0, "1", 0, "0" }, 1, "NFSU2_QUIT_FRAMES" },
            { { 0, 0, "100", "1", 0, "0" }, 1, "QUITFRAMES" },
            { { 0, 0, 0, 0, 0, "0" }, 1, "headless (no WINDOW)" },
            { { 0, 0, 0, "1", "2", "0" }, 1, "NOFOCUS=2 test window" },
            { { 0, 0, 0, "1", 0, "60" }, 1, "a timeout" },
            { { 0, 0, 0, "1", 0, 0 }, 1, "ALARM unset (boot.c: 20 s)" },
        };
        for (size_t i = 0; i < sizeof t / sizeof *t; i++){
            for (int k = 0; vars[k]; k++){ if (t[i].env[k]) setenv(vars[k], t[i].env[k], 1); else unsetenv(vars[k]); }
            const char *why = automated_run();
            CHECK(!!why == t[i].automated, "guard %-40s -> %s", t[i].what, why ? why : "play session");
        }
        /* discovery: with TMPDIR (and the rest) pointing at a private dir, no
         * candidate lies outside it and /tmp; in particular not the per-user
         * dir confstr reports, where the real Discord's socket is */
        setenv("TMPDIR", tmp, 1); setenv("XDG_RUNTIME_DIR", tmp, 1); unsetenv("TMP"); unsetenv("TEMP");
        g_ipc_path[0] = 0;
        static char cand[400][DS_SUN];
        int n = ipc_candidates(cand, 400), outside = 0;
        for (int i = 0; i < n; i++)
            if (strncmp(cand[i], tmp, strlen(tmp)) && strncmp(cand[i], "/tmp/", 5)){ outside++; printf("INFO outside: %s\n", cand[i]); }
        CHECK(n > 0 && !outside, "guard discovery stays in $TMPDIR and /tmp when TMPDIR is set (%d candidates, %d outside)", n, outside);
        return g_fail;
    }
    mem_setup(); g_session_start = (int64_t)time(0);

    if (!strcmp(c, "ipc")){
        mock_start(py, tmp, c, "normal", 0);
        ipc_env(2);
        sit_menus();  run_frames(1.0);
        sit_custom(); run_frames(1.0);
        sit_loading(); run_frames(0.6);
        /* a race whose second line changes every frame (position flapping) */
        for (int k = 0; k < 120; k++){ sit_race(k / 40, 1 + k % 4); run_frames(0.025); }
        /* a burst of first-line changes faster than Discord allows */
        for (int k = 0; k < 16; k++){ if (k & 1) sit_menus(); else sit_custom(); run_frames(0.2); }
        /* odd characters through to the wire */
        sit_roam(); memcpy(g_mem + G_PROFILE, "A\"B\\\x01\xE9", 7); run_frames(DS_RATE_WIN + 1.0);
        sit_menus(); run_frames(DS_RATE_WIN + 1.0);      /* the last state must win (coalescing) */
        double ms = timed_shutdown();
        CHECK(ms < DS_QUIT_WAIT_MS, "ipc quit took %.0f ms", ms);
        CHECK(g_max_call_us < 1000, "ipc longest game-thread call %.0f us CPU (wall %.0f us; first call, which "
              "starts the IPC thread, %.0f us)", g_max_call_us, g_max_wall_us, g_init_us);
        usleep(200000); mock_stop(SIGTERM);
        printf("INFO ipc frames in %s\n", g_log_path);
        return g_fail;
    }
    if (!strcmp(c, "bench")){
        /* cost of the Present hook per frame: off, and on between samples */
        setenv("NFSU2_DISCORD", "0", 1); g_on = -1;
        discord_on_present();                               /* reads the option once */
        long n = 0; double a = mono_s(), ca = thread_cpu_us();
        while (mono_s() - a < 1.0){ for (int k = 0; k < 1000; k++) discord_on_present(); n += 1000; }
        double off_ns = (thread_cpu_us() - ca) * 1e3 / n;
        snprintf(g_sock, sizeof g_sock, "%s/bench.sock", tmp); unlink(g_sock);
        ipc_env(0); g_on = -1;
        sit_race(1, 2);
        discord_on_present();                               /* init + first sample */
        n = 0; a = mono_s(); ca = thread_cpu_us();
        while (mono_s() - a < 2.0){ for (int k = 0; k < 1000; k++) discord_on_present(); n += 1000; }
        double on_ns = (thread_cpu_us() - ca) * 1e3 / n;
        printf("INFO bench: off %.1f ns per Present; on %.1f ns per Present on average over %ld calls in 2 s "
               "(%d samples a second, avg %.1f us each)\n", off_ns, on_ns, n, DS_SAMPLE_HZ,
               g_cost_n ? g_cost_total / g_cost_n * 1e6 : 0.0);
        CHECK(off_ns < 20 && on_ns < 1000, "bench: per-frame cost negligible");
        timed_shutdown();
        return g_fail;
    }
    if (!strcmp(c, "absent")){
        snprintf(g_sock, sizeof g_sock, "%s/absent.sock", tmp); unlink(g_sock);
        ipc_env(1);
        sit_menus(); run_frames(0.2);
        struct rusage a, b; getrusage(RUSAGE_SELF, &a);
        sleep(5);                                         /* the game thread is idle too: measure the IPC thread */
        getrusage(RUSAGE_SELF, &b);
        double cpu = (b.ru_utime.tv_sec - a.ru_utime.tv_sec) * 1e3 + (b.ru_utime.tv_usec - a.ru_utime.tv_usec) / 1e3 +
                     (b.ru_stime.tv_sec - a.ru_stime.tv_sec) * 1e3 + (b.ru_stime.tv_usec - a.ru_stime.tv_usec) / 1e3;
        CHECK(cpu < 20.0, "absent: %.1f ms CPU in 5 s with no Discord (retry every %.1f s)", cpu, DS_RETRY_MAX);
        CHECK(g_c.fd < 0, "absent: not connected");
        double ms = timed_shutdown();
        CHECK(ms < 50, "absent: quit took %.1f ms", ms);
        return g_fail;
    }
    if (!strcmp(c, "late")){
        snprintf(g_sock, sizeof g_sock, "%s/%s.sock", tmp, c); unlink(g_sock);
        ipc_env(1);
        sit_menus(); run_frames(2.0);
        CHECK(g_c.fd < 0, "late: nothing to connect to yet");
        double t = mono_s();
        mock_start(py, tmp, c, "normal", 0);
        while (!g_c.ready && mono_s() - t < 5) run_frames(0.05);
        CHECK(g_c.ready, "late: connected %.2f s after Discord appeared (backoff max %.1f s)", mono_s() - t, DS_RETRY_MAX);
        run_frames(0.5);
        timed_shutdown(); usleep(200000); mock_stop(SIGTERM);
        return g_fail;
    }
    if (!strcmp(c, "restart")){
        mock_start(py, tmp, c, "normal", 0);
        ipc_env(1);
        sit_race(1, 2); run_frames(1.0);
        CHECK(g_c.ready, "restart: connected");
        mock_stop(SIGKILL);                                /* Discord crashes / quits */
        double t = mono_s();
        while (g_c.fd >= 0 && mono_s() - t < 3) run_frames(0.05);
        CHECK(g_c.fd < 0, "restart: noticed Discord went away after %.2f s", mono_s() - t);
        run_frames(1.0);
        mock_start(py, tmp, c, "normal", 1);               /* Discord comes back */
        t = mono_s();
        while (!g_c.ready && mono_s() - t < 5) run_frames(0.05);
        CHECK(g_c.ready, "restart: reconnected %.2f s after Discord came back", mono_s() - t);
        run_frames(1.0);
        CHECK(g_max_call_us < 1000, "restart: longest game-thread call %.0f us CPU (wall %.0f us)", g_max_call_us, g_max_wall_us);
        timed_shutdown(); usleep(200000); mock_stop(SIGTERM);
        return g_fail;
    }
    if (!strcmp(c, "park")){
        /* the app parks the game in the background: the game thread stops
         * (no Present, no samples) and lifecycle.c tells discord.c */
        mock_start(py, tmp, c, "normal", 0);
        ipc_env(2);
        sit_race(1, 2); run_frames(1.0);                   /* race clock at 30 s */
        g_test_parked = 1; discord_lifecycle_changed();
        usleep(4000000);                                   /* parked 4 s: the race clock stands still */
        g_test_parked = 0; discord_lifecycle_changed();
        sit_race(1, 2); run_frames(1.0);                   /* resumed: the clock moves on from 30 s */
        g_test_parked = 1; discord_lifecycle_changed();    /* a short park (< 3 s): no re-anchor, */
        usleep(1000000);                                   /* so no fresh sample: shown again after */
        g_test_parked = 0; discord_lifecycle_changed();    /* DS_UNPARK_S */
        usleep((useconds_t)(DS_UNPARK_S * 1e6) + 300000);
        sit_menus(); run_frames(DS_RATE_WIN + 1.0);
        timed_shutdown(); usleep(200000); mock_stop(SIGTERM);
        return g_fail;
    }
    if (!strcmp(c, "noread")){
        /* Discord stops reading: the client's writes block once the socket
         * buffer is full; only the IPC thread may wait (1 s), then reconnect */
        mock_start(py, tmp, c, c, 0);
        ipc_env(1);
        for (int k = 0; k < 12; k++){ if (k & 1) sit_menus(); else sit_custom(); run_frames(0.5); }
        CHECK(g_max_call_us < 1000, "noread: longest game-thread call %.0f us CPU (wall %.0f us)", g_max_call_us, g_max_wall_us);
        double ms = timed_shutdown();
        CHECK(ms <= DS_QUIT_WAIT_MS + 50, "noread: quit took %.0f ms (bound %d ms)", ms, DS_QUIT_WAIT_MS);
        usleep(100000); mock_stop(SIGTERM);
        return g_fail;
    }
    /* misbehaving Discord */
    if (!strcmp(c, "noready") || !strcmp(c, "close") || !strcmp(c, "garbage") || !strcmp(c, "slow") ||
        !strcmp(c, "error") || !strcmp(c, "badid")){
        mock_start(py, tmp, c, c, 0);
        ipc_env(2);
        sit_menus(); run_frames(1.5);
        sit_race(0, 3); run_frames(2.5);
        CHECK(g_max_call_us < 1000, "%s: longest game-thread call %.0f us CPU (wall %.0f us)", c, g_max_call_us, g_max_wall_us);
        double ms = timed_shutdown();
        CHECK(ms <= DS_QUIT_WAIT_MS + 50, "%s: quit took %.0f ms (bound %d ms)", c, ms, DS_QUIT_WAIT_MS);
        usleep(100000); mock_stop(SIGTERM);
        return g_fail;
    }
    printf("FAIL unknown case %s\n", c);
    return 2;
}
