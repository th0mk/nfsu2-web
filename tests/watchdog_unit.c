/* watchdog_unit.c -- tests for runtime/watchdog.c without the game.
 *
 *   clang -O1 -g -Wall -Wextra -Wno-unused-parameter -Iruntime -o out/watchdog_unit tests/watchdog_unit.c
 *   out/watchdog_unit                          (about 8 s)
 *
 * The watchdog compares its own clock with stamps other threads took: the
 * last Present (game thread) and each guest thread's since_us.  In the
 * browser those threads are other workers, and a stamp taken a moment ago
 * can read a little ahead of the watchdog's now; 2026-10-25/26 logs had
 * "[watchdog] no Present for 18446744073709.6 s" (2^64 us) from that.
 * Cases, on the real watchdog thread (NFSU2_WATCHDOG=1; no game flow reader,
 * so game flow 0 and the loading screen's limit of 20 s):
 *   ahead-us   the game thread keeps stamping a few microseconds ahead: no report
 *   ahead-s    a stamp 2 s ahead: no report
 *   stall      the last Present 25 s ago: one report, "no Present for 25..28 s",
 *              a thread whose since_us is ahead reads "for 0.0 s"
 *   lines      a game table that names no threads (UG2's) keeps the thread line
 *              as it always was: no name, no routine, no symbol after the eip
 *              (Most Wanted's names: tests/mw_harness_unit.c threads)
 *   short      5 s ago (under the limit): no report
 *   download   the page's Download log: a stamp ahead is "0.0 s ago" and no
 *              report; 5 s ago is "5.0 s ago" with the report
 * Every case prints "PASS <case> ..." or "FAIL <case> ..."; exit 1 on a FAIL. */
#include "../runtime/watchdog.c"
#include <string.h>
#include <sys/stat.h>

/* ---- stand-ins for win32_ext.c, mem.c, debug.c and the game's table ------- */
const GameOps g_game = { .id = "ug2", .flow_fe = 3u, .flow_race = 6u };   /* no flow(): game flow 0 */
TState   g_tstate[MAXCPUS];
CPU     *g_all_cpus[MAXCPUS];
uint32_t g_all_tids[MAXCPUS];
uint32_t g_all_start[MAXCPUS];
int      g_all_n;
CPU     *g_cur_cpu;
uint8_t  g_cbits[1u << 15] = {0};               /* nothing committed: game flow reads 0 */
uint8_t *g_mem;
void guest_backtrace(CPU *c, int n){ (void)c; (void)n; }
uint32_t guest_nearest_fn(uint32_t a){ return a; }              /* would name any eip: the table names no threads */
uint32_t module_nearest_fn(uint32_t a, const char **m){ *m = "x.dll"; return a; }
int  win32_cs_held(uint32_t *key, uint32_t *owner, int *depth, int max){ return 0; }
int  win32_obj_describe(uint32_t h, char *buf, size_t n){ return snprintf(buf, n, "handle 0x%08x", h); }

static int g_fail;
#define CHECK(cond, ...) do{ if (cond) printf("PASS " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); g_fail = 1; } \
                             printf("\n"); fflush(stdout); }while(0)

/* stderr goes to a file; err_since() returns what was written after a mark */
static FILE *g_err;
static off_t g_mark;
static off_t err_size(void){ struct stat st; fflush(stderr); return fstat(fileno(g_err), &st) == 0 ? st.st_size : 0; }
static void err_mark(void){ g_mark = err_size(); }
static char *err_since(void){
    static char buf[16384];
    size_t n = (size_t)(err_size() - g_mark);
    if (n >= sizeof buf) n = sizeof buf - 1;
    ssize_t got = pread(fileno(g_err), buf, n, g_mark);
    buf[got > 0 ? got : 0] = 0;
    return buf;
}
static void wait_ms(int ms){ struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, 0); }

/* the game thread presenting with its clock a few microseconds ahead */
static _Atomic int g_stamping;
static void *stamper(void *arg){
    (void)arg;
    while (atomic_load(&g_stamping)) atomic_store(&g_wd_present_us, wd_now_us() + 5);
    return 0;
}
/* guest thread 0 stamping its since_us as far ahead, until the report has read
 * it: a stamp taken once would read as old as the wait for the watchdog's next
 * tick (0..1 s) */
static _Atomic int g_tstamping;
static void *tstamper(void *arg){
    (void)arg;
    while (atomic_load(&g_tstamping)) g_tstate[0].since_us = wd_now_us() + 5;
    return 0;
}

int main(void){
    g_err = tmpfile();
    if (!g_err || dup2(fileno(g_err), 2) < 0){ printf("FAIL setup: no temporary file for stderr\n"); return 1; }
    setvbuf(stderr, 0, _IONBF, 0);
    static CPU cpu;
    g_all_cpus[0] = &cpu; g_all_tids[0] = 101; g_all_start[0] = 0x0075bcc7u; g_all_n = 1;
    g_tstate[0].what = TS_WAIT; g_tstate[0].obj = 0x00A00002u;

    setenv("NFSU2_WATCHDOG", "1", 1);
    watchdog_start();

    /* ahead-us: what the browser does now and then, kept up for two ticks */
    err_mark();
    atomic_store(&g_stamping, 1);
    pthread_t th; pthread_create(&th, 0, stamper, 0);
    wait_ms(2500);
    atomic_store(&g_stamping, 0); pthread_join(th, 0);
    CHECK(watchdog_reports() == 0 && !strstr(err_since(), "no Present"),
          "ahead-us: stamps 5 us ahead for 2.5 s give no report (%llu reports)", (unsigned long long)watchdog_reports());

    /* ahead-s: a stamp well ahead */
    err_mark();
    atomic_store(&g_wd_present_us, wd_now_us() + 2000000u);
    wait_ms(2500);
    CHECK(watchdog_reports() == 0 && !strstr(err_since(), "no Present"),
          "ahead-s: a stamp 2 s ahead gives no report (%llu reports)", (unsigned long long)watchdog_reports());

    /* stall: a real one still reports, once */
    err_mark();
    atomic_store(&g_tstamping, 1);                  /* this thread's own stamp, ahead as well */
    pthread_create(&th, 0, tstamper, 0);
    atomic_store(&g_wd_present_us, wd_now_us() - 25000000u);
    for (int i = 0; i < 30 && watchdog_reports() == 0; i++) wait_ms(100);
    wait_ms(1500);                                  /* a second tick: still one report */
    atomic_store(&g_tstamping, 0); pthread_join(th, 0);
    {   const char *e = err_since(); double s = -1, f = -1;
        const char *p = strstr(e, "no Present for "), *q = strstr(e, "waiting on ");
        if (p) sscanf(p, "no Present for %lf", &s);
        if (q && (q = strstr(q, " for "))) sscanf(q, " for %lf", &f);
        CHECK(watchdog_reports() == 1 && s >= 25.0 && s < 28.0,
              "stall: 25 s without a Present reports once (%llu reports, %.1f s)", (unsigned long long)watchdog_reports(), s);
        CHECK(f == 0.0, "stall: a thread stamp ahead reads 0.0 s in the report (%.1f s)", f);
        const char *l = strstr(e, "  thread 0 ");
        CHECK(l && !strncmp(l, "  thread 0 tid=101: waiting on handle 0x00a00002 for 0.0 s, esp=0x00000000 eip=0x00000000\n", 89),
              "lines: the thread line as it always was (%.89s)", l ? l : "none"); }

    /* short: under the loading screen's 20 s */
    err_mark();
    atomic_store(&g_wd_present_us, wd_now_us() - 5000000u);
    wait_ms(1500);
    CHECK(watchdog_reports() == 1 && !strstr(err_since(), "no Present"),
          "short: 5 s without a Present gives no report (%llu reports)", (unsigned long long)watchdog_reports());

    /* download: the page's Download log */
    err_mark();
    atomic_store(&g_wd_present_us, wd_now_us() + 5);
    watchdog_log_download();
    {   const char *e = err_since();
        CHECK(strstr(e, "the last frame 0.0 s ago") && !strstr(e, "no Present") && watchdog_reports() == 1,
              "download: a stamp ahead is 0.0 s ago, no report"); }
    err_mark();
    g_tstate[0].since_us = wd_now_us() + 5;
    atomic_store(&g_wd_present_us, wd_now_us() - 5000000u);
    watchdog_log_download();
    {   const char *e = err_since(); double f = -1;
        const char *q = strstr(e, "waiting on ");
        if (q && (q = strstr(q, " for "))) sscanf(q, " for %lf", &f);
        CHECK(strstr(e, "the last frame 5.0 s ago") && strstr(e, "no Present for 5.0 s") && watchdog_reports() == 2 && f == 0.0,
              "download: 5 s ago is reported, the thread stamp ahead reads 0.0 s (%.1f s)", f); }

    printf(g_fail ? "watchdog unit tests: FAILED\n" : "watchdog unit tests: all passed\n");
    return g_fail;
}
