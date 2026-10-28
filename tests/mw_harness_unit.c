/* mw_harness_unit.c -- Most Wanted's boot count, harness and thread names
 * without the game (TRACKER M2-17, M2-18, M2-20): MW's real game table
 * (runtime/mw/game_mw.c, fe_mw.c, threads_mw.c) on the real dispatch.c and
 * watchdog.c, over guest memory laid out as speed.exe's.
 *
 *   node --test web/test/mw-harness.test.mjs      (builds and runs it natively)
 *
 * The test writes out/web/test/mw_harness_gen.c from the exe and the lift:
 * the two initializer tables as their words (0x8EA000-0x8EB198), the lifted
 * _initterm (sub_007ca540) and _initterm_e (sub_007ca570) as gen/mw/all has
 * them, and a function table (g_fn_addr) that gives every function of the
 * tables a stand-in that counts its calls (the global 0x9AA9D0's constructor
 * 0x889DD0 also sets the bitset pointer its body sets, [0x9AB21C] =
 * 0x9A2DE0), plus a few function starts for the watchdog's symbols.
 *
 * Modes (argv[1]); each prints "PASS <mode>: ..." or "FAIL <mode>: ...":
 *   init      the lifted runners run the tables as _cinit (0x7CA2C0) does, with
 *             the count on (initcount_install): one [init] line, 5 of 5 and 870
 *             of 870, the check set; every function ran once; a call into a
 *             constructor from elsewhere ran but did not count; the table entries
 *             are back afterwards and the stack balanced
 *   stop      the third C initializer returns non-zero: _initterm_e stops, the
 *             line comes at once (2 of 5, stopped at slot 0x8EB174), C++ not run
 *   nobitset  the constructor leaves its pointer 0: the line says NULL
 *   exit      the boot stops before the C++ table: the exit report's line says so
 *   fe        GameFlow [0x925E90], the top package and the screens of every frame
 *             (fe_mw.c) over a front end built as FEngine keeps it
 *   threads   mw_thread_start past _threadstartex and the game's trampoline, and a
 *             forced stall report that names each thread, its routine, wait, eip
 *             as a function and offset, and the GIL holder (L-073) */
#include "../runtime/watchdog.c"
#include "module.h"
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* ---- guest memory: the first 16 MB, all of it committed ---------------- */
#define GUEST_SPAN 0x01000000u
uint8_t  g_cbits[1u << 15];
uint8_t *g_mem;
#define STACK_TOP 0x00f00000u
#define RET_CINIT 0x007ca2e9u            /* _cinit's return address after its _initterm_e call */

/* ---- win32_ext.c's, for the watchdog ----------------------------------- */
TState   g_tstate[MAXCPUS];
CPU     *g_all_cpus[MAXCPUS];
uint32_t g_all_tids[MAXCPUS];
uint32_t g_all_start[MAXCPUS];
int      g_all_n;
CPU     *g_cur_cpu;
int win32_cs_held(uint32_t *key, uint32_t *owner, int *depth, int max){
    if (max < 1) return 0;
    key[0] = 0x009c4000u; owner[0] = 104; depth[0] = 1; return 1;
}
int win32_obj_describe(uint32_t h, char *buf, size_t n){
    return snprintf(buf, n, "handle 0x%08x (timer, manual reset, signaled 0, due in 4.0 ms, period 0.0 ms)", h);
}
/* ---- dispatch.c's: no Win32, COM, generated code or modules here ------- */
int g_efx_stat;
uint32_t g_watch_prev_ip;
void (*g_backtrace_fn)(struct CPU *, int);
int  efx_watched(uint32_t a){ (void)a; return 0; }
void efx_dispatch(CPU *c, uint32_t a){ (void)c; (void)a; }
int  win32_call(CPU *c, uint32_t i){ (void)c; (void)i; return 0; }
int  com_call(CPU *c, uint32_t a){ (void)c; (void)a; return 0; }
int  run_generated(CPU *c, uint32_t a){ (void)c; (void)a; return 0; }
void genexec_report(void){ }
void autotest_note_trap(uint32_t a, const char *m){ (void)a; (void)m; }
mod_fn module_fn_at(uint32_t a){ (void)a; return 0; }
uint32_t module_fn_from_host(const void *pc){ (void)pc; return 0; }
/* server.dll at 0x00D00000 (MW's slot), one function at 0x00D01000 */
uint32_t module_nearest_fn(uint32_t a, const char **mod){
    if (a >= 0x00d01000u && a < 0x00d80000u){ *mod = "server.dll"; return 0x00d01000u; }
    return 0;
}
/* ---- game_mw.c's: what this unit does not link --------------------------- */
void GAMEINFO_ENTRY_FN(CPU *c){ (void)c; }
int  win32_misc_call(CPU *c, const char *dll, const char *n, uint32_t *r){ (void)c; (void)dll; (void)n; (void)r; return 0; }
const RegKey mw_reg_keys[] = { { 0, 0, 0, 0 } };
const ModInfo g_srv_modinfo = { "", "", 0, 0, 0, 0, 0, 0, 0 };
const ImpEnt g_srv_imp[1]; const uint32_t g_srv_imp_count = 0;
const uint32_t g_srv_fn_addr[1]; const mod_fn g_srv_fn_ptr[1]; const uint32_t g_srv_fn_count = 0;
/* ---- fe_mw.c's: the screens it notes ------------------------------------ */
static char g_noted[16][64]; static int g_nnoted;
void autotest_note_screen(const char *nm){ if (g_nnoted < 16) snprintf(g_noted[g_nnoted++], 64, "%s", nm); }

/* ---- mw_harness_gen.c's -------------------------------------------------- */
extern const uint32_t mwt_tables[];      /* the words of 0x8EA000-0x8EB198 */
extern const uint32_t mwt_ntables;
void sub_007ca540(CPU *c); void sub_007ca570(CPU *c);   /* the lifted _initterm and _initterm_e */
void initcount_install(void); void initcount_report(void); void dispatch_init(void);
uint32_t mw_thread_start(uint32_t entry, uint32_t arg);
extern const ThreadName mw_thread_names[];

/* the functions of the tables (mw_harness_gen.c's g_fn_ptr) */
static long g_xi_calls, g_xc_calls; static int g_fail_at, g_nobitset;
void xi_stub(CPU *c){                     /* a C initializer: 0 = it worked */
    uint32_t r; POP32(r); c->eip = r;
    g_xi_calls++;
    c->eax = g_fail_at && g_xi_calls == g_fail_at ? 0x1au : 0u;
}
void xc_stub(CPU *c){ uint32_t r; POP32(r); c->eip = r; g_xc_calls++; }
void bitset_ctor(CPU *c){                 /* 0x889DD0: 0x9AA9D0's constructor, which sets +0x84C */
    if (!g_nobitset) MR32(0x009aa9d0u + 0x84cu) = 0x009a2de0u;
    xc_stub(c);
}
void named_stub(CPU *c){ uint32_t r; POP32(r); c->eip = r; }

static int g_fail;
#define CHECK(cond, ...) do{ if (cond) printf("PASS " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); g_fail = 1; } \
                             printf("\n"); fflush(stdout); }while(0)
/* stderr goes to a file; err_since() is what was written after err_mark() */
static FILE *g_err; static off_t g_mark;
static off_t err_size(void){ struct stat st; fflush(stderr); return fstat(fileno(g_err), &st) == 0 ? st.st_size : 0; }
static void err_mark(void){ g_mark = err_size(); }
static char *err_since(void){
    static char buf[65536];
    size_t n = (size_t)(err_size() - g_mark);
    if (n >= sizeof buf) n = sizeof buf - 1;
    ssize_t got = pread(fileno(g_err), buf, n, g_mark);
    buf[got > 0 ? got : 0] = 0;
    return buf;
}
/* the line of text that starts with head, copied (or "") */
static const char *line_of(const char *text, const char *head){
    static char l[1024]; l[0] = 0;
    const char *p = strstr(text, head);
    if (p){ size_t n = strcspn(p, "\n"); if (n >= sizeof l) n = sizeof l - 1; memcpy(l, p, n); l[n] = 0; }
    return l;
}

/* how many times needle is in text */
static int count(const char *text, const char *needle){
    int n = 0;
    for (const char *p = text; (p = strstr(p, needle)) != 0; p += strlen(needle)) n++;
    return n;
}

static CPU g_cpu;
/* a runner called as _cinit calls it: push hi, push lo, call (cdecl: the caller pops) */
static void run_table(void (*runner)(CPU *), uint32_t lo, uint32_t hi, uint32_t ret){
    CPU *c = &g_cpu;
    PUSH32(hi); PUSH32(lo); PUSH32(ret);
    runner(c);
    c->esp += 8u;
}
static void boot_tables(int both){
    memcpy(g_mem + 0x008ea000u, mwt_tables, mwt_ntables * 4u);
    memset(&g_cpu, 0, sizeof g_cpu);
    g_cpu.esp = STACK_TOP;
    initcount_install();
    run_table(sub_007ca570, 0x008eb168u, 0x008eb180u, RET_CINIT);
    if (both && !g_cpu.eax) run_table(sub_007ca540, 0x008ea000u, 0x008eb164u, 0x007ca316u);
}
static int nonnull(uint32_t lo, uint32_t hi){
    int n = 0;
    for (uint32_t a = lo; a < hi; a += 4u) n += MR32(a) != 0;
    return n;
}
/* every function of the tables: the table calls it directly again (not through dispatch_indirect) */
static int entries_back(void){
    for (uint32_t a = 0x008ea000u; a < 0x008eb180u; a += 4u){
        uint32_t f = MR32(a);
        if (f && !dispatch_fast(f)) return 0;
    }
    return 1;
}

static void test_init(void){
    memcpy(g_mem + 0x008ea000u, mwt_tables, mwt_ntables * 4u);
    int want_xi = nonnull(0x008eb168u, 0x008eb180u), want_xc = nonnull(0x008ea000u, 0x008eb164u);
    err_mark();
    memset(&g_cpu, 0, sizeof g_cpu);
    g_cpu.esp = STACK_TOP;
    initcount_install();
    CHECK(!dispatch_fast(MR32(0x008ea004u)) && !dispatch_fast(0x00889dd0u),
          "init: while counting, the tables' functions are out of the dispatch table");
    run_table(sub_007ca570, 0x008eb168u, 0x008eb180u, RET_CINIT);
    CHECK(g_cpu.eax == 0 && g_cpu.esp == STACK_TOP, "init: _initterm_e returned 0, the stack is balanced (esp 0x%08x)", g_cpu.esp);
    /* a call into a constructor from elsewhere (another return address, edx not a slot) runs but does not count */
    {   CPU *c = &g_cpu; long before = g_xc_calls;
        c->edx = 0x12345678u; PUSH32(0x00401234u);
        dispatch_indirect(c, MR32(0x008ea004u));
        CHECK(g_xc_calls == before + 1 && c->esp == STACK_TOP, "init: a constructor called from elsewhere ran"); }
    run_table(sub_007ca540, 0x008ea000u, 0x008eb164u, 0x007ca316u);
    CHECK(g_cpu.esp == STACK_TOP, "init: _initterm returned, the stack is balanced (esp 0x%08x)", g_cpu.esp);
    const char *e = err_since(), *l = line_of(e, "[init]");
    char want[512];
    snprintf(want, sizeof want, "[init] %d of %d C initializers (__xi 0x008eb168-0x008eb180, 6 slots, by 0x007ca570 from 0x007ca2e4), "
             "%d of %d C++ constructors (__xc 0x008ea000-0x008eb164, 1113 slots, by 0x007ca540 from 0x007ca311); "
             "[0x009ab21c] = 0x009a2de0, set", want_xi, want_xi, want_xc, want_xc);
    printf("line: %s\n", l);
    CHECK(!strcmp(l, want), "init: the [init] line (want: %s)", want);
    CHECK(count(e, "[init]") == 1, "init: one [init] line (%d)", count(e, "[init]"));
    CHECK(g_xi_calls == want_xi && g_xc_calls == want_xc + 1, "init: each function ran once (%ld C, %ld C++ with the extra call)",
          g_xi_calls, g_xc_calls);
    CHECK(entries_back(), "init: afterwards the tables' functions are in the dispatch table again");
    err_mark(); initcount_report();
    CHECK(!strstr(err_since(), "[init]"), "init: the exit report prints no second line");
}
static void test_stop(void){
    g_fail_at = 3;
    err_mark();
    boot_tables(1);
    const char *l = line_of(err_since(), "[init]");
    printf("line: %s\n", l);
    CHECK(g_cpu.eax == 0x1au && g_xi_calls == 3 && g_xc_calls == 0, "stop: _initterm_e stopped at the third (eax 0x%x, %ld C, %ld C++)",
          g_cpu.eax, g_xi_calls, g_xc_calls);
    CHECK(strstr(l, "[init] 3 of 5 C initializers (__xi 0x008eb168-0x008eb180, 6 slots, by 0x007ca570 from 0x007ca2e4; "
                    "stopped: slot 0x008eb174 returned 0x0000001a), 0 of 870 C++ constructors (") && strstr(l, "; not run)")
          && strstr(l, "[0x009ab21c] = 0: NULL"), "stop: the line names the slot and the table not run");
    CHECK(entries_back(), "stop: the tables' functions are in the dispatch table again");
}
static void test_nobitset(void){
    g_nobitset = 1;
    err_mark();
    boot_tables(1);
    const char *l = line_of(err_since(), "[init]");
    printf("line: %s\n", l);
    CHECK(strstr(l, "870 of 870 C++ constructors") && strstr(l, "; [0x009ab21c] = 0: NULL, a constructor did not run its body"),
          "nobitset: the check reads NULL");
}
static void test_exit(void){
    err_mark();
    boot_tables(0);                           /* the boot stops after the C initializers */
    CHECK(!strstr(err_since(), "[init]"), "exit: no line while the C++ table has not run");
    initcount_report();
    const char *l = line_of(err_since(), "[init]");
    printf("line: %s\n", l);
    CHECK(strstr(l, "[init] 5 of 5 C initializers") && strstr(l, "0 of 870 C++ constructors") && strstr(l, "; not run)")
          && strstr(l, "; at exit: the tables did not finish"), "exit: the exit report's line says the C++ table did not run");
    CHECK(entries_back(), "exit: the tables' functions are in the dispatch table again");
}

/* ---- the front end, as FEngine keeps it (fe_mw.c) ------------------------ */
#define CFENG   0x00a00000u
#define ENGINE  0x00a10000u
#define PKG(i)  (0x00a20000u + 0x100u * (uint32_t)(i))
#define STR(i)  (0x00a30000u + 0x40u * (uint32_t)(i))
static uint32_t hash(const char *s){                 /* bStringHash, upper case (0x5AF1C0) */
    uint32_t h = 0xffffffffu;
    for (; *s; s++){ uint8_t ch = (uint8_t)*s; if (ch >= 'a' && ch <= 'z') ch -= 0x20; h = h * 33u + ch; }
    return h;
}
static void pkg(int i, const char *name){
    snprintf((char *)(g_mem + STR(i)), 0x40, "%s", name);
    MR32(PKG(i) + 0xcu) = STR(i); MR32(PKG(i) + 0x10u) = hash(name);
}
/* the active list: these packages, head to tail */
static void active(int n, const int *ix){
    MR32(ENGINE + 0xdcu) = (uint32_t)n;
    MR32(ENGINE + 0xe0u) = n ? PKG(ix[0]) : 0; MR32(ENGINE + 0xe4u) = n ? PKG(ix[n - 1]) : 0;
    for (int k = 0; k < n; k++){
        MR32(PKG(ix[k]) + 4u) = k + 1 < n ? PKG(ix[k + 1]) : 0;
        MR32(PKG(ix[k]) + 8u) = k ? PKG(ix[k - 1]) : 0;
    }
}
static const char *noted(void){
    static char b[512]; b[0] = 0;
    for (int i = 0; i < g_nnoted; i++){ strcat(b, i ? ", " : ""); strcat(b, g_noted[i]); }
    g_nnoted = 0;
    return b;
}
static void frame(void){ g_game.on_test_frame(); }
static void test_fe(void){
    CHECK(game_flow() == 0 && !g_game.top_screen(), "fe: nothing there yet: flow 0, no top package");
    MR32(0x00925e90u) = 3;
    MR32(0x0091cadcu) = CFENG; MR32(CFENG) = ENGINE;
    CHECK(game_flow() == 3 && game_flow() == g_game.flow_fe && g_game.flow_race == 6, "fe: GameFlow [0x925E90] = 3 is the front end");
    frame();
    CHECK(!g_game.top_screen() && !*noted(), "fe: an empty package list: no top package, no screen");
    pkg(0, "WS_MW_LS_Splash.fng"); pkg(1, "MainMenu.fng"); pkg(2, "MainMenu_Sub.fng"); pkg(3, "Chyron_FE.fng");
    active(1, (int[]){ 0 }); frame();
    const char *t = g_game.top_screen(), *s = noted();
    CHECK(t && !strcmp(t, "WS_MW_LS_Splash.fng") && !strcmp(s, "WS_MW_LS_Splash.fng"), "fe: the splash: top %s, screens noted: %s", t ? t : "-", s);
    frame();
    CHECK(!*noted(), "fe: the next frame notes nothing new");
    active(3, (int[]){ 3, 1, 2 }); frame();
    t = g_game.top_screen(); s = noted();
    CHECK(t && !strcmp(t, "MainMenu_Sub.fng") && !strcmp(s, "Chyron_FE.fng, MainMenu.fng, MainMenu_Sub.fng"),
          "fe: the main menu: the tail is on top (%s), the new ones noted head to tail: %s", t ? t : "-", s);
    pkg(1, "Options.fng"); frame();                 /* another package where MainMenu.fng was */
    s = noted();
    CHECK(!strcmp(s, "Options.fng"), "fe: a new package at a freed one's address is noted: %s", s);
    MR32(PKG(2) + 4u) = PKG(2); frame();            /* a list that loops: the walk ends at its bound */
    s = noted();
    CHECK(!*s, "fe: a list that loops: the walk ended, nothing new noted (%s)", s);
    MR32(PKG(2) + 0xcu) = 0x00a40000u; memcpy(g_mem + 0x00a40000u, "not a package", 14);
    CHECK(!g_game.top_screen(), "fe: a name that is not a package's reads as none");
    MR32(0x00925e90u) = 6;
    CHECK(game_flow() == 6 && game_flow() == g_game.flow_race, "fe: GameFlow 6 is racing");
}

/* ---- the threads (threads_mw.c, watchdog.c) ------------------------------ */
static const char *name_of(uint32_t r){ const char *n = game_thread_name(r); return n ? n : "-"; }
static void test_threads(void){
    /* CreateThread's entry and argument, as each start site gives them */
    CHECK(mw_thread_start(0x00820bd7u, 0x009c22ecu) == 0x00820bd7u, "threads: a direct start (0x820C82): its own routine");
    uint32_t ptd = 0x00a50000u, blk = 0x00a60000u;
    MR32(ptd + 0x4cu) = 0x00835670u; MR32(ptd + 0x50u) = 0x00a70000u;
    CHECK(mw_thread_start(0x0085d6e0u, ptd) == 0x00835670u, "threads: _beginthreadex (0x83572E): the routine at +0x4C");
    MR32(ptd + 0x4cu) = 0x007ec1d3u; MR32(ptd + 0x50u) = blk;
    MR32(blk) = 0; MR32(blk + 4u) = 0x007ec601u; MR32(blk + 8u) = 0;
    CHECK(mw_thread_start(0x0085d6e0u, ptd) == 0x007ec601u, "threads: the game's maker (0x7EC44F): the block's routine");
    CHECK(mw_thread_start(0x0085d6e0u, 0x7ff00000u) == 0x0085d6e0u, "threads: a data block it cannot read: the entry");
    MR32(ptd + 0x4cu) = 0;
    CHECK(mw_thread_start(0x0085d6e0u, ptd) == 0x0085d6e0u, "threads: no routine in the block: the entry");
    CHECK(!strcmp(name_of(0x007c4040u), "main") && !strcmp(name_of(0x007ec601u), "timer") && !strcmp(name_of(0x00820bd7u), "sound")
          && !strcmp(name_of(0x0084ac10u), "socket") && !strcmp(name_of(0x007ef80au), "prefetch") && !strcmp(name_of(0x00401000u), "-"),
          "threads: the names of GAME.md 6.2's routines");

    /* a forced stall report: the game's threads as they wait, each eip a return address of its routine's
     * import call (the timer's WaitForSingleObject at 0x7EC60D, the sound thread's Sleep at 0x820BE7, the
     * socket thread's WSAWaitForMultipleEvents through its thunk at 0x84AD1C) */
    static CPU cpu[8];
    struct { uint32_t tid, start; int what; uint32_t obj, eip, esp; } th[] = {
        {   1, 0x007c4040u, TS_RUN,   0,           0x007c408fu, 0x00e00000u },   /* holds the GIL */
        { 102, 0x007ec601u, TS_WAIT,  0x00a00004u, 0x007ec613u, 0x00e10000u },
        { 103, 0x00820bd7u, TS_SLEEP, 8,           0x00820be9u, 0x00e20000u },
        { 104, 0x0084ac10u, TS_WAITM, 0x00a00008u, 0x0084ad21u, 0x00e30000u },
        { 105, 0,           TS_GIL,   0,           0x008356a4u, 0x00e40000u },   /* _beginthreadex's: below */
        { 106, 0x00d0a000u, TS_CS,    0x009c4000u, 0x00d0a010u, 0x00e50000u },   /* a server.dll thread */
        { 107, 0,           TS_SLEEP, 1,           0,           0x00e60000u },   /* no routine known */
    };
    MR32(ptd + 0x4cu) = 0x00835670u;
    th[4].start = mw_thread_start(0x0085d6e0u, ptd);
    int n = (int)(sizeof th / sizeof th[0]);
    for (int i = 0; i < n; i++){
        cpu[i].eip = th[i].eip; cpu[i].esp = th[i].esp;
        g_all_cpus[i] = &cpu[i]; g_all_tids[i] = th[i].tid; g_all_start[i] = th[i].start;
        g_tstate[i].what = th[i].what; g_tstate[i].obj = th[i].obj; g_tstate[i].since_us = wd_now_us() - 2000000u;
    }
    MR32(th[1].esp + 8u) = 0x007ec1f2u;            /* the timer thread's stack: the trampoline's return */
    g_all_n = n; g_cur_cpu = &cpu[0];
    MR32(0x00925e90u) = 3;
    err_mark();
    wd_report(25.0, game_flow());
    const char *e = err_since();
    printf("%s", e);
    CHECK(strstr(e, "[watchdog] no Present for 25.0 s (game flow 3, ") && strstr(e, "[watchdog] end of report"), "threads: the report");
    static const char *const want[] = {
        "  thread 0 tid=1 main (0x007c4040) (holds the GIL): running  for 2.0 s, esp=0x00e00000 eip=0x007c408f (sub_007c4040+0x4f)",
        "  thread 1 tid=102 timer (0x007ec601): waiting on handle 0x00a00004 (timer, manual reset, signaled 0, due in 4.0 ms, "
        "period 0.0 ms) for 2.0 s, esp=0x00e10000 eip=0x007ec613 (sub_007ec601+0x12)",
        "  thread 2 tid=103 sound (0x00820bd7): sleeping 8 ms for 2.0 s, esp=0x00e20000 eip=0x00820be9 (sub_00820bd7+0x12)",
        "  thread 3 tid=104 socket (0x0084ac10): waiting on (multiple) first handle 0x00a00008 (timer, manual reset, signaled 0, "
        "due in 4.0 ms, period 0.0 ms) for 2.0 s, esp=0x00e30000 eip=0x0084ad21 (sub_0084ac10+0x111)",
        "  thread 4 tid=105 lobby (0x00835670): waiting for the GIL  for 2.0 s, esp=0x00e40000 eip=0x008356a4 (sub_00835670+0x34)",
        "  thread 5 tid=106 (0x00d0a000): in contended critical section 0x009c4000 for 2.0 s, esp=0x00e50000 eip=0x00d0a010 "
        "(server.dll sub_00d01000+0x9010)",
        "  thread 6 tid=107 (routine not known): sleeping 1 ms for 2.0 s, esp=0x00e60000 eip=0x00000000",
    };
    static const char *const what[] = { "the main thread: its routine, the GIL holder, its eip in the entry point",
        "the timer thread waits on its timer", "the sound thread sleeps", "the socket thread waits on its events",
        "a thread _beginthreadex started, by the routine it runs", "a server.dll thread, unnamed, its eip in the DLL",
        "a thread with no routine known (the WINMM timer's kind)" };
    for (int i = 0; i < n; i++){
        char head[24]; snprintf(head, sizeof head, "  thread %d ", i);
        const char *l = line_of(e, head);
        CHECK(!strcmp(l, want[i]), "threads: %s", what[i]);
        if (strcmp(l, want[i])) printf("  got:  %s\n  want: %s\n", l, want[i]);
    }
    CHECK(count(e, "(holds the GIL)") == 1, "threads: one GIL holder");
    CHECK(strstr(e, "return 0x007ec1f2   (sub_007ec1d3 + 0x1f)") != 0, "threads: the timer thread's stack, symbolised");
    CHECK(strstr(e, "critical section 0x009c4000 held by tid 104 (depth 1)") != 0, "threads: the critical section's owner");
}

int main(int argc, char **argv){
    const char *mode = argc > 1 ? argv[1] : "";
    g_mem = mmap(0, GUEST_SPAN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_mem == MAP_FAILED){ printf("FAIL setup: no guest memory\n"); return 1; }
    memset(g_cbits, 0xff, GUEST_SPAN / 0x4000u / 8u);
    g_err = tmpfile();
    if (!g_err || dup2(fileno(g_err), 2) < 0){ printf("FAIL setup: no temporary file for stderr\n"); return 1; }
    setvbuf(stderr, 0, _IONBF, 0);
    dispatch_init();
    if (!strcmp(mode, "init")) test_init();
    else if (!strcmp(mode, "stop")) test_stop();
    else if (!strcmp(mode, "nobitset")) test_nobitset();
    else if (!strcmp(mode, "exit")) test_exit();
    else if (!strcmp(mode, "fe")) test_fe();
    else if (!strcmp(mode, "threads")) test_threads();
    else { printf("usage: mw_harness_unit init|stop|nobitset|exit|fe|threads\n"); return 2; }
    printf(g_fail ? "mw_harness_unit %s: FAILED\n" : "mw_harness_unit %s: all passed\n", mode);
    return g_fail;
}
