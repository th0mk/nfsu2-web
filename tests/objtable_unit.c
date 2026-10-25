/* objtable_unit.c -- tests for the kernel object table of runtime/win32_ext.c
 * (CreateThread, CreateEventA, waits, CloseHandle) without the game.
 *
 *   clang -O1 -g -Wall -Wno-unused-parameter -Wno-unused-function -Iruntime -o out/objtable_unit tests/objtable_unit.c
 *   out/objtable_unit [case ...]               (all cases: under a second)
 *
 * The game calls the real import handlers (win32_ext_call) from guest threads
 * that hold the guest lock; the "game" here is this file's main thread and
 * the guest threads it starts, whose entries are stand-ins (dispatch_indirect):
 * E_RET returns at once, E_HOLD runs until the test releases it.  SPEED2.EXE
 * starts a thread for every race or world load and never touches its handle
 * again, so each load leaves one slot of the table's 4,095 behind.  Each case
 * runs in a process of its own:
 *   below    under the limit nothing changes: ended threads keep their slots,
 *            handles count up from H_BASE + 1, a closed slot is reused as before
 *   reclaim  8 slots left, then 40 such threads one after the other: every
 *            CreateThread and CreateEventA still gets a handle, the table
 *            never overflows; in the exit report the reclaimed slots and the
 *            ended threads still in the table add up to the 40
 *   oldest   the slot that goes first is the one of the thread that ended first
 *   stale    a reclaimed thread's old handle names nothing: a wait on it returns
 *            at once as for an ended thread, CloseHandle on it leaves the slot's
 *            new owner (a running thread) alone
 *   kept     threads whose handle was waited on (WaitForSingleObject or
 *            WaitForMultipleObjects), duplicated or asked for its exit code,
 *            and running threads, keep their slots: a full table refuses a new
 *            object as before
 *   waiting  a thread that ends while WaitForMultipleObjects waits on it keeps
 *            its slot: the wait looked its handle up before it gave the guest
 *            lock away (a second thread holds the wait at its first handle
 *            until a full table has asked for a slot)
 * Every case prints "PASS <case> ..." or "FAIL <case> ..." lines; exit 1 on a FAIL. */
#include "../runtime/win32_ext.c"
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* ---- stand-ins for the rest of the runtime --------------------------------- */
uint8_t *g_mem;
uint8_t  g_cbits[1u << 15] = {0};
CPU     *g_cur_cpu;
void     audio_set_muted(int m){ (void)m; }
double   audio_time(void){ return 0.0; }
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args){ return 0; }
void     com_forget_cpu(CPU *c){ (void)c; }
int      hfr_mode_off(void){ return 1; }
uint64_t host_us(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u; }
uint32_t host_ms(void){ return (uint32_t)(host_us() / 1000u); }
void     kbd_pump(void){}
const char *language_registry(void){ return "English"; }
int      lifecycle_may_park(void){ return 0; }
void     lifecycle_pump(void){}
void     mem_altstack_install(void){}
void     mem_commit(uint32_t a, uint32_t n){ (void)a; (void)n; }    /* g_mem is mapped read-write whole */
long     metal_frames(void){ return 0; }
void     metal_game_size(unsigned *w, unsigned *h){ *w = 1280; *h = 720; }
void     metal_get_cursor(int *x, int *y){ *x = *y = 0; }
uint32_t module_nearest_fn(uint32_t a, const char **m){ return 0; }
uint32_t res_find(uint32_t a, uint32_t b, int k){ return 0; }
uint32_t res_data(uint32_t a){ return 0; }
uint32_t res_size(uint32_t a){ return 0; }
int      win32_caller_module(void){ return 0; }       /* SPEED2.EXE's imports */
uint32_t win32_import_va(const char *n){ (void)n; return 0; }
void     win32_set_last_error(CPU *c, uint32_t e){ (void)c; (void)e; }
const GameOps g_game = { .id = "ug2", .exe_name = "SPEED2.EXE", .vroot = "C:\\NFSU2" };

static int g_fail;
#define CHECK(cond, ...) do{ if (cond) printf("PASS " __VA_ARGS__); else { printf("FAIL " __VA_ARGS__); g_fail = 1; } \
                             printf("\n"); fflush(stdout); }while(0)

/* an import call as the lifted code makes it: stdcall arguments on the guest stack */
static uint32_t imp(CPU *c, const char *n, int na, ...){
    va_list ap; va_start(ap, na);
    c->esp -= 4u * (uint32_t)na;
    for (int i = 0; i < na; i++) MR32(c->esp + 4u * (uint32_t)i) = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t r = 0;
    if (!win32_ext_call(c, n, &r)){ printf("FAIL import %s not handled\n", n); exit(2); }
    c->esp += 4u * (uint32_t)na;
    return r;
}

/* the guest threads' entries */
enum { E_RET = 0x00401000u, E_HOLD = 0x00402000u };
static int g_release[64];                       /* E_HOLD: its argument indexes this */
void dispatch_indirect(CPU *c, uint32_t entry){
    if (entry == E_HOLD){
        uint32_t k = MR32(c->esp + 4u) & 63u;    /* [esp] the return address, [esp+4] the argument */
        while (!__atomic_load_n(&g_release[k], __ATOMIC_ACQUIRE)) imp(c, "Sleep", 1, 1u);
    }
    c->eax = 0;
}

static CPU g_main;                              /* the game's main thread: this one */
#define M (&g_main)
#define SCRATCH 0x7E000000u                     /* guest memory for out-parameters */
#define INFINITE 0xFFFFFFFFu
#define WAIT_TIMEOUT 0x102u

/* CreateThread from the call site SPEED2.EXE uses for its per-load thread */
static uint32_t start(uint32_t entry, uint32_t arg){
    M->eip = 0x00726478u;
    return imp(M, "CreateThread", 6, 0u, 0u, entry, arg, 0u, 0u);
}
/* let the other guest threads run until n threads have ended in all */
static int ended(uint64_t n){
    for (int i = 0; i < 5000 && win32_ext_threads_exited() < n; i++) imp(M, "Sleep", 1, 1u);
    return win32_ext_threads_exited() >= n;
}
/* events until only `left` slots of the table are free */
static void fill(int left){
    while (win32_obj_live() < OBJ_MAX - 1 - left && imp(M, "CreateEventA", 4, 0u, 1u, 0u, 0u)) {}
}
static uint32_t slot_of(uint32_t h){ return (h - H_BASE) % OBJ_MAX; }

/* stderr goes to a file; err_has() looks for text written after err_mark() */
static FILE *g_err;
static off_t g_mark;
static off_t err_size(void){ struct stat st; fflush(stderr); return fstat(fileno(g_err), &st) == 0 ? st.st_size : 0; }
static void err_mark(void){ g_mark = err_size(); }
static const char *err_text(void){
    static char buf[1 << 16];
    off_t end = err_size(), from = end - g_mark > (off_t)sizeof buf - 1 ? end - (off_t)sizeof buf + 1 : g_mark;
    ssize_t n = pread(fileno(g_err), buf, (size_t)(end - from), from);
    buf[n > 0 ? n : 0] = 0;
    return buf;
}
static int err_has(const char *s){ return strstr(err_text(), s) != 0; }

/* ---- cases ------------------------------------------------------------------ */
static void case_below(void){
    uint32_t h[20]; int ok = 1;
    for (int i = 0; i < 20; i++){ h[i] = start(E_RET, 0); ok &= h[i] == H_BASE + 1u + (uint32_t)i && ended((uint64_t)i + 1); }
    CHECK(ok && win32_obj_live() == 20, "below: 20 ended threads keep 20 slots, handles 0x%08x..0x%08x (%d in use)",
          h[0], h[19], win32_obj_live());
    imp(M, "CloseHandle", 1, h[4]);
    uint32_t e = imp(M, "CreateEventA", 4, 0u, 1u, 0u, 0u);
    CHECK(win32_obj_live() == 20 && e == h[4], "below: a closed thread's slot goes to the next object, same handle (0x%08x)", e);
}

static void case_reclaim(void){
    fill(8);
    int live0 = win32_obj_live(), ok = 1, peak = 0;
    err_mark();
    for (int i = 0; i < 40; i++){
        uint32_t h = start(E_RET, 0);
        if (!h){ printf("  thread %d: CreateThread gave NULL\n", i + 1); ok = 0; break; }
        if (!ended((uint64_t)i + 1)){ printf("  thread %d did not end\n", i + 1); ok = 0; break; }
        uint32_t e = imp(M, "CreateEventA", 4, 0u, 1u, 0u, 0u);
        if (!e){ printf("  after thread %d: CreateEventA gave NULL\n", i + 1); ok = 0; break; }
        imp(M, "CloseHandle", 1, e);
        if (win32_obj_live() > peak) peak = win32_obj_live();
    }
    CHECK(ok && peak <= OBJ_MAX - 1 && !err_has("object table full ("),
          "reclaim: 40 threads on 8 free slots all got handles, events too (%d -> %d in use, peak %d of %d)",
          live0, win32_obj_live(), peak, OBJ_MAX - 1);
    /* each slot a thread took back went to the next thread or event: the
     * 40 threads are the reclaimed ones and those still in the table */
    err_mark();
    win32_obj_report();
    const char *t = err_text(), *p = strstr(t, "full 0, "), *q = strstr(t, "ended threads     : ");
    unsigned rec = 0, open = 0, grp = 0; char tail[160] = "";
    if (p) sscanf(p, "full 0, %u reclaimed", &rec);
    if (q) sscanf(q, "ended threads     : %u with the handle still open: %u x %159[^\n]", &open, &grp, tail);
    CHECK(rec > 0 && rec + open == 40 && grp == open && open <= 8
          && !strcmp(tail, "entry 0x00401000 from 0x00726478 (handles looked up 0 times)"),
          "reclaim: the exit report: %u reclaimed, %u ended threads with an open handle (%u x %s)", rec, open, grp, tail);
}

static void case_oldest(void){
    fill(3);
    uint32_t h[3];
    for (int k = 0; k < 3; k++) h[k] = start(E_HOLD, (uint32_t)k);
    int order[3] = { 2, 0, 1 }, ok = h[0] && h[1] && h[2];
    for (int j = 0; j < 3 && ok; j++){ __atomic_store_n(&g_release[order[j]], 1, __ATOMIC_RELEASE); ok = ended((uint64_t)j + 1); }
    uint32_t n[3];
    for (int j = 0; j < 3; j++){ n[j] = start(E_RET, 0); ok &= n[j] != 0 && ended(4u + (uint64_t)j); }
    CHECK(ok && slot_of(n[0]) == slot_of(h[2]) && slot_of(n[1]) == slot_of(h[0]) && slot_of(n[2]) == slot_of(h[1]),
          "oldest: slots go back in the order the threads ended (ended 3,1,2; new 0x%08x 0x%08x 0x%08x, old 0x%08x 0x%08x 0x%08x)",
          n[0], n[1], n[2], h[0], h[1], h[2]);
    CHECK(n[0] == h[2] + OBJ_MAX && n[1] == h[0] + OBJ_MAX, "oldest: the new handle is the next generation of the slot");
}

static void case_stale(void){
    fill(1);
    uint32_t a = start(E_RET, 0);
    int ok = a && ended(1);
    err_mark();
    uint32_t b = ok ? start(E_HOLD, 5) : 0;     /* the table is full: b takes a's slot */
    CHECK(ok && b && slot_of(b) == slot_of(a) && b != a && err_has("goes to a new object"),
          "stale: a full table gives the ended thread's slot to the next thread (0x%08x -> 0x%08x)", a, b);
    if (!b) return;
    uint64_t t0 = host_us();
    uint32_t w0 = imp(M, "WaitForSingleObject", 2, a, 0u), wi = imp(M, "WaitForSingleObject", 2, a, INFINITE);
    double ms = (double)(host_us() - t0) / 1000.0;
    uint32_t wb = imp(M, "WaitForSingleObject", 2, b, 0u);
    CHECK(w0 == 0 && wi == 0 && ms < 100.0 && wb == WAIT_TIMEOUT,
          "stale: a wait on the old handle returns at once as for an ended thread (%u, %u, %.1f ms), the new thread runs on (%u)",
          w0, wi, ms, wb);
    int live = win32_obj_live();
    uint32_t r = imp(M, "CloseHandle", 1, a);
    uint32_t wb2 = imp(M, "WaitForSingleObject", 2, b, 0u);
    CHECK(r == 1 && win32_obj_live() == live && wb2 == WAIT_TIMEOUT,
          "stale: CloseHandle on the old handle leaves the new thread's slot alone (%d -> %d in use, wait %u)",
          live, win32_obj_live(), wb2);
    __atomic_store_n(&g_release[5], 1, __ATOMIC_RELEASE);
    int e = ended(2);
    CHECK(e && imp(M, "WaitForSingleObject", 2, b, INFINITE) == 0, "stale: the new thread ends and its handle says so");
}

static void case_kept(void){
    fill(5);
    uint32_t joined = start(E_RET, 0), dup = start(E_RET, 0), code = start(E_RET, 0), multi = start(E_RET, 0), run = start(E_HOLD, 7);
    int ok = joined && dup && code && multi && run && ended(4);
    ok &= imp(M, "WaitForSingleObject", 2, joined, INFINITE) == 0;          /* the usual join */
    MR32(SCRATCH + 8u) = multi;
    ok &= imp(M, "WaitForMultipleObjects", 4, 1u, SCRATCH + 8u, 0u, 0u) == 0;
    imp(M, "DuplicateHandle", 7, 0xFFFFFFFFu, dup, 0xFFFFFFFFu, SCRATCH, 0u, 0u, 2u);
    imp(M, "GetExitCodeThread", 2, code, SCRATCH + 4u);
    err_mark();
    uint32_t e = imp(M, "CreateEventA", 4, 0u, 1u, 0u, 0u);
    CHECK(ok && e == 0 && err_has("object table full (") && win32_obj_live() == OBJ_MAX - 1,
          "kept: joined, duplicated, exit-code-polled, multi-waited and running threads keep their slots; the full table refuses (0x%08x)", e);
    CHECK(imp(M, "WaitForSingleObject", 2, joined, 0u) == 0 && imp(M, "WaitForSingleObject", 2, dup, 0u) == 0
          && imp(M, "WaitForSingleObject", 2, code, 0u) == 0 && imp(M, "WaitForSingleObject", 2, multi, 0u) == 0
          && imp(M, "WaitForSingleObject", 2, run, 0u) == WAIT_TIMEOUT,
          "kept: their handles still name them");
    __atomic_store_n(&g_release[7], 1, __ATOMIC_RELEASE);
    ended(5);
}

/* waiting: a second "guest" thread with a CPU and stack of its own; it holds
 * the event's mutex, so the wait's loop stops at its first handle, and asks
 * for an object once the thread the wait is for has ended */
static CPU g_help;
static uint32_t g_wev, g_wnew;
static int g_wgo;
static void *waiting_helper(void *p){
    Obj *ev = &g_obj[slot_of(g_wev)];
    pthread_mutex_lock(&ev->mtx);
    __atomic_store_n(&g_wgo, 1, __ATOMIC_RELEASE);
    guest_lock(&g_help);                        /* only once the wait has given the guest lock away */
    __atomic_store_n(&g_release[9], 1, __ATOMIC_RELEASE);
    for (int i = 0; i < 5000 && win32_ext_threads_exited() < 1; i++) imp(&g_help, "Sleep", 1, 1u);
    g_wnew = imp(&g_help, "CreateEventA", 4, 0u, 1u, 0u, 0u);
    guest_unlock();
    pthread_mutex_unlock(&ev->mtx);
    return p;
}
static void case_waiting(void){
    fill(2);
    uint32_t t = start(E_HOLD, 9);
    g_wev = imp(M, "CreateEventA", 4, 0u, 0u, 0u, 0u);                     /* the table is full now */
    g_help.esp = 0x7E800000u - 0x1000u;
    pthread_t th;
    if (!t || !g_wev || pthread_create(&th, 0, waiting_helper, 0)){ CHECK(0, "waiting: set-up (0x%08x 0x%08x)", t, g_wev); return; }
    while (!__atomic_load_n(&g_wgo, __ATOMIC_ACQUIRE)) sched_yield();      /* the guest lock stays here till the wait */
    MR32(SCRATCH + 8u) = g_wev; MR32(SCRATCH + 12u) = t;
    err_mark();
    uint32_t w = imp(M, "WaitForMultipleObjects", 4, 2u, SCRATCH + 8u, 0u, INFINITE);
    guest_yield_begin(); pthread_join(th, 0); guest_yield_end();          /* it may still want the guest lock */
    CHECK(w == 1 && g_wnew == 0 && err_has("object table full (") && !err_has("goes to a new object"),
          "waiting: the thread a wait was for keeps its slot when it ends during the wait; the full table refuses (0x%08x),"
          " the wait sees it end (%u)", g_wnew, w);
}

static const struct { const char *name; void (*fn)(void); } CASES[] = {
    { "below", case_below }, { "reclaim", case_reclaim }, { "oldest", case_oldest }, { "stale", case_stale }, { "kept", case_kept },
    { "waiting", case_waiting },
};

static int run_case(int k){
    g_mem = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (g_mem == MAP_FAILED){ printf("FAIL %s: no guest memory\n", CASES[k].name); return 1; }
    g_err = tmpfile();
    if (!g_err || dup2(fileno(g_err), 2) < 0){ printf("FAIL %s: no temporary file for stderr\n", CASES[k].name); return 1; }
    setvbuf(stderr, 0, _IONBF, 0);
    g_main.esp = 0x7F000000u - 0x1000u;
    guest_lock(&g_main);
    CASES[k].fn();
    return g_fail;
}

int main(int argc, char **argv){
    int n = (int)(sizeof CASES / sizeof CASES[0]), fail = 0;
    for (int k = 0; k < n; k++){
        int want = argc < 2;
        for (int a = 1; a < argc; a++) want |= !strcmp(argv[a], CASES[k].name);
        if (!want) continue;
        fflush(stdout);
        pid_t pid = fork();                       /* one process per case: a fresh table */
        if (pid == 0) _exit(run_case(k));
        int st = 0; waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0){
            fail = 1;
            if (!WIFEXITED(st) || WEXITSTATUS(st) > 1) printf("FAIL %s: the case's process ended with status 0x%x\n", CASES[k].name, st);
        }
    }
    printf(fail ? "object table unit tests: FAILED\n" : "object table unit tests: all passed\n");
    return fail;
}
