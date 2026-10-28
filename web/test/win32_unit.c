/* The Win32 layer's imports of the Most Wanted HLE tasks (TRACKER M2-09,
 * M2-10, M2-11), as lifted code calls them: arguments pushed right to left, a
 * return address, then win32_call with the import's index, exactly what a
 * `call [iat]` does.  runtime/win32.c, win32_ext.c, win32_misc.c and
 * keyboard.c are the real ones; the rest of the runtime is stubbed below.
 *
 *   win32_unit.wasm hle      single-threaded (node's WASI, web/test/win32-wasm.test.mjs):
 *                            every import against Windows' documented answers; after each
 *                            call the stack pointer is back above the arguments (stdcall:
 *                            the table's count, which the test file checks against
 *                            argcount's KNOWN and MW's lift), the return address is taken
 *                            and the words above the arguments are untouched
 *   win32_unit.wasm stress   the waits with threads (the browser host, web/tools/waitstress.sh):
 *                            waitable timers under 8 threads, a shared periodic timer, a
 *                            WaitForMultipleObjects ping-pong and a handle closed mid-wait;
 *                            every wait finite, a lost wake-up is a timeout and fails
 *   win32_unit_ug2.wasm ug2  built with -DWIN32_UNIT_UG2: UG2's game table, which leaves
 *                            g_game.win32_misc 0; the imports UG2 has of win32_misc.c's stay
 *                            unimplemented, as UG2 always had them (web/test/win32-wasm.test.mjs)
 *
 * Prints a line per failed check and "win32_unit: <n> passed, <m> failed". */
#include "cpu.h"
#include "game.h"
#include "kbd.h"
#include "module.h"
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static _Atomic int pass, fail;
#define CHECK(cond, ...) do { if (cond) pass++; else { fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while (0)

/* ---- the import table: Windows' stdcall argument counts (SDK signatures, GAME.md 3.3) ---- */
const ImpEnt g_imp[] = {
#define IAT(i) (0x00500000u + 4u * (i))
    /* M2-09 */
    { IAT(0),  "KERNEL32.dll", "CreateWaitableTimerA", 3 },
    { IAT(1),  "KERNEL32.dll", "SetWaitableTimer", 6 },
    { IAT(2),  "KERNEL32.dll", "CancelWaitableTimer", 1 },
    { IAT(3),  "KERNEL32.dll", "WaitForSingleObject", 2 },
    { IAT(4),  "KERNEL32.dll", "WaitForMultipleObjects", 4 },
    { IAT(5),  "KERNEL32.dll", "GetSystemTime", 1 },
    { IAT(6),  "KERNEL32.dll", "SystemTimeToFileTime", 2 },
    { IAT(7),  "KERNEL32.dll", "CreateEventA", 4 },
    { IAT(8),  "KERNEL32.dll", "SetEvent", 1 },
    { IAT(9),  "KERNEL32.dll", "ResetEvent", 1 },
    { IAT(10), "KERNEL32.dll", "CloseHandle", 1 },
    { IAT(11), "KERNEL32.dll", "GetLastError", 0 },
    /* M2-10 */
    { IAT(12), "KERNEL32.dll", "GlobalMemoryStatusEx", 1 },
    { IAT(13), "KERNEL32.dll", "GetDateFormatA", 6 },
    { IAT(14), "KERNEL32.dll", "GetTimeFormatA", 6 },
    { IAT(15), "KERNEL32.dll", "CompareStringA", 6 },
    { IAT(16), "KERNEL32.dll", "CompareStringW", 6 },
    { IAT(17), "KERNEL32.dll", "SetStdHandle", 2 },
    { IAT(18), "KERNEL32.dll", "GetStdHandle", 1 },
    { IAT(19), "KERNEL32.dll", "SetEnvironmentVariableA", 2 },
    { IAT(20), "KERNEL32.dll", "GetEnvironmentVariableA", 3 },
    { IAT(21), "USER32.dll",   "GetKeyState", 1 },
    { IAT(22), "USER32.dll",   "GetKeyboardLayout", 1 },
    { IAT(23), "USER32.dll",   "MapVirtualKeyA", 2 },
    { IAT(24), "USER32.dll",   "MapVirtualKeyExA", 3 },
    { IAT(25), "USER32.dll",   "ToUnicode", 6 },
    { IAT(26), "USER32.dll",   "SendInput", 3 },
    { IAT(27), "KERNEL32.dll", "QueueUserAPC", 3 },
    { IAT(28), "KERNEL32.dll", "CreateProcessA", 10 },
    { IAT(29), "KERNEL32.dll", "FatalAppExitA", 2 },
    /* M2-11 */
    { IAT(30), "WINMM.dll", "waveOutGetDevCapsA", 3 },
    { IAT(31), "WINMM.dll", "waveOutGetNumDevs", 0 },
    { IAT(32), "WINMM.dll", "waveOutWrite", 3 },
    { IAT(33), "WINMM.dll", "waveOutGetPosition", 3 },
    { IAT(34), "WINMM.dll", "waveOutSetVolume", 2 },
    { IAT(35), "WINMM.dll", "waveOutReset", 1 },
    { IAT(36), "WINMM.dll", "waveOutOpen", 6 },
    { IAT(37), "WINMM.dll", "waveOutClose", 1 },
    { IAT(38), "WINMM.dll", "waveOutPrepareHeader", 3 },
    { IAT(39), "WINMM.dll", "waveOutUnprepareHeader", 3 },
    { IAT(40), "WINMM.dll", "waveInStart", 1 },
    { IAT(41), "WINMM.dll", "waveInStop", 1 },
    { IAT(42), "WINMM.dll", "waveInReset", 1 },
    { IAT(43), "WINMM.dll", "waveInAddBuffer", 3 },
    { IAT(44), "WINMM.dll", "waveInGetPosition", 3 },
    { IAT(45), "WINMM.dll", "waveInOpen", 6 },
    { IAT(46), "WINMM.dll", "waveInClose", 1 },
    { IAT(47), "WINMM.dll", "waveInGetNumDevs", 0 },
    { IAT(48), "WINMM.dll", "waveInGetDevCapsA", 3 },
    { IAT(49), "WINMM.dll", "waveInPrepareHeader", 3 },
    { IAT(50), "WINMM.dll", "waveInUnprepareHeader", 3 },
    { IAT(51), "DSOUND.dll", "#6", 3 },
};
const uint32_t g_imp_count = sizeof g_imp / sizeof *g_imp;

/* ---- guest memory: [0, the program's data) is the guest's (the link's global base) ---- */
uint8_t g_cbits[1u << 15];
#define STACK_TOP  0x00800000u           /* the main thread's guest stack; threads below it */
#define TEB_MAIN   0x00700000u
#define SCRATCH    0x00600000u
#define RET_ADDR   0x00401234u
#define H_TIMER0   0x00a00000u           /* memmap.h: object handles start here (UG2's map) */

/* ---- runtime stand-ins: what win32*.c and keyboard.c call outside the four files ---- */
/* the game's table: Most Wanted's answers win32_misc.c's imports; built with -DWIN32_UNIT_UG2 it is UG2's,
 * which leaves g_game.win32_misc 0 (the "ug2" half) */
int win32_misc_call(CPU *c, const char *dll, const char *n, uint32_t *r);
#ifndef WIN32_UNIT_UG2
const GameOps g_game = { .id = "test", .exe_name = "GAME.EXE", .vroot = "C:\\GAME", .module_path = "C:\\GAME\\GAME.EXE",
                         .win32_misc = win32_misc_call };
#else
const GameOps g_game = { .id = "test", .exe_name = "GAME.EXE", .vroot = "C:\\GAME", .module_path = "C:\\GAME\\GAME.EXE" };
#endif
CPU *g_cur_cpu;
void mem_commit(uint32_t a, uint32_t n){ (void)a; (void)n; }
void mem_altstack_install(void){ }
uint32_t g_image_base = 0x00400000u, g_image_size = 0x00200000u;
int memmap_check(uint32_t lo, uint32_t hi, int fatal){ (void)lo; (void)hi; (void)fatal; return 0; }
void guest_backtrace(CPU *c, int n){ (void)c; (void)n; }
void dispatch_indirect(CPU *c, uint32_t a){ (void)c; (void)a; }
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args){ (void)c; (void)fn; (void)nargs; (void)args; return 0; }
void heap_init(void){ }
void heap_report(void){ }
int heap_api_kind(const char *name){ (void)name; return 0; }
uint32_t heap_api(CPU *c, int kind){ (void)c; (void)kind; return 0; }
static uint32_t g_bump = 0x00300000u;
uint32_t gheap_alloc(int heap, uint32_t n, int zero){ (void)heap; uint32_t p = g_bump; g_bump += (n + 15u) & ~15u; if (zero) memset(g_mem + p, 0, n); return p; }
uint32_t gheap_usize(uint32_t p){ (void)p; return 0; }
void heap_totals(uint64_t *live, uint64_t *peak, uint64_t *va, uint64_t *held){ if (live) *live = 0; if (peak) *peak = 0; if (va) *va = 0; if (held) *held = 0; }
int modheap_owns(uint32_t h){ (void)h; return 0; }
uint32_t modheap_create(int mod){ (void)mod; return 0; }
int modheap_destroy(uint32_t h){ (void)h; return 0; }
uint32_t modheap_alloc(uint32_t h, uint32_t n, uint32_t f){ (void)h; (void)n; (void)f; return 0; }
int modheap_free(uint32_t h, uint32_t u){ (void)h; (void)u; return 0; }
uint32_t modheap_realloc(uint32_t h, uint32_t u, uint32_t n, uint32_t f){ (void)h; (void)u; (void)n; (void)f; return 0; }
uint32_t modheap_size(uint32_t h, uint32_t u){ (void)h; (void)u; return 0; }
void modheap_report(void){ }
void module_init(void){ }
int module_count(void){ return 0; }
const ModLink *module_link(int m){ (void)m; return 0; }
uint32_t module_imp_first(int m){ (void)m; return 0; }
int module_of_import(uint32_t idx){ (void)idx; return 0; }
int module_known_name(const char *nm){ (void)nm; return 0; }
uint32_t module_by_name(const char *nm){ (void)nm; return 0; }
uint32_t module_load(CPU *c, const char *nm, uint32_t *err){ (void)c; (void)nm; (void)err; return 0; }
int module_handle(uint32_t h){ (void)h; return 0; }
uint32_t module_proc(uint32_t h, uint32_t name, uint32_t *err){ (void)h; (void)name; (void)err; return 0; }
int module_free(CPU *c, uint32_t h){ (void)c; (void)h; return 0; }
int module_file_name(uint32_t h, char *out, int n){ (void)h; (void)out; (void)n; return 0; }
void module_note_socket_call(int m, const char *n, uint32_t a, uint32_t r){ (void)m; (void)n; (void)a; (void)r; }
void module_report(void){ }
uint32_t module_nearest_fn(uint32_t addr, const char **mod){ (void)addr; (void)mod; return 0; }
int winsock_call(CPU *c, const char *dll, const char *n, uint32_t *r){ (void)c; (void)dll; (void)n; (void)r; return 0; }
void winsock_report(void){ }
int fileio_call(CPU *c, const char *n, uint32_t *r){ (void)c; (void)n; (void)r; return 0; }
int fileio_is_handle(uint32_t h){ (void)h; return 0; }
void fileio_report(void){ }
uint32_t com_direct3dcreate9(void){ return 0; }
uint32_t com_directinput8create(uint32_t a){ (void)a; return 0; }
uint32_t com_directsoundcreate(uint32_t a){ (void)a; return 0; }
void com_init(void){ }
void com_forget_cpu(CPU *c){ (void)c; }
void watchdog_start(void){ }
void res_report(void){ }
uint32_t res_find(uint32_t a, uint32_t b, int w){ (void)a; (void)b; (void)w; return 0; }
uint32_t res_data(uint32_t a){ (void)a; return 0; }
uint32_t res_size(uint32_t a){ (void)a; return 0; }
void overrides_report(void){ }
void enh_report(void){ }
void d3dx_hle_report(void){ }
void discord_shutdown(void){ }
long metal_frames(void){ return 0; }
void metal_game_size(int *w, int *h){ if (w) *w = 640; if (h) *h = 480; }
void metal_get_cursor(int *x, int *y){ if (x) *x = 0; if (y) *y = 0; }
int hfr_display_live(void){ return 1; }
int hfr_mode_off(void){ return 1; }
uint64_t hfr_vclock_floor_us(void){ return 4000u; }
const char *language_registry(void){ return 0; }
void lifecycle_pump(void){ }
int lifecycle_may_park(void){ return 0; }
int lifecycle_game_parked(void){ return 0; }
void audio_set_muted(int m){ (void)m; }
double audio_time(void){ return 0; }
const char *nfsu2_tzname[2] = { "UTC", "UTC" };
struct tm *nfsu2_localtime_r(const time_t *t, struct tm *out){ return gmtime_r(t, out); }
struct tm *nfsu2_localtime(const time_t *t){ static struct tm s; return gmtime_r(t, &s); }
time_t nfsu2_mktime(struct tm *tm){ return timegm(tm); }
void (*g_report_fn)(void);
uint32_t g_watch_prev_ip;
void watch_arm(uint32_t a){ (void)a; }
void mprot_arm(uint32_t a){ (void)a; }
static char g_unimpl[64];
void autotest_note_unimpl(const char *dll, const char *name, uint32_t eip){ (void)dll; (void)eip; snprintf(g_unimpl, sizeof g_unimpl, "%s", name); }

/* ---- the runtime's entry points the test drives ---- */
int win32_call(CPU *c, uint32_t idx);
void win32_init(void);
void guest_lock(CPU *c);
void guest_unlock(void);
void vclock_tick(void);
uint64_t host_us(void);
extern _Atomic uint64_t g_vclock_freeze;
int win32_obj_describe(uint32_t h, char *buf, size_t n);
extern jmp_buf g_exit_jmp; extern int g_exit_code, g_exit_armed;

/* ---- calling an import as lifted code does ---- */
typedef struct { CPU cpu; uint32_t stack_top; } Thr;
static int g_want_unimpl;            /* the "ug2" half: the import must reach win32.c's not-implemented path */
static uint32_t idx_of(const char *name){
    for (uint32_t i = 0; i < g_imp_count; i++) if (!strcmp(g_imp[i].name, name)) return i;
    printf("FAIL: no import %s in the table\n", name); exit(2);
}
static uint32_t call_imp(Thr *t, const char *name, ...){
    uint32_t i = idx_of(name), n = (uint32_t)g_imp[i].nargs, a[16];
    va_list ap; va_start(ap, name);
    for (uint32_t k = 0; k < n; k++) a[k] = va_arg(ap, uint32_t);
    va_end(ap);
    CPU *c = &t->cpu;
    uint32_t base = t->stack_top - 0x100u;
    for (uint32_t k = 0; k < 8; k++) MR32(base + 4u * k) = 0xC0DE0000u + k;   /* the caller's frame, above the arguments */
    c->esp = base;
    for (uint32_t k = n; k-- > 0; ) PUSH32(a[k]);
    PUSH32(RET_ADDR);
    g_unimpl[0] = 0;
    win32_call(c, i);
    int ok = 1;
    for (uint32_t k = 0; k < 8; k++) ok &= MR32(base + 4u * k) == 0xC0DE0000u + k;
    CHECK(c->esp == base, "%s: esp 0x%08x after the call, 0x%08x expected (%u arguments)", name, c->esp, base, n);
    CHECK(c->eip == RET_ADDR, "%s: return address 0x%08x", name, c->eip);
    CHECK(ok, "%s: wrote into the caller's frame", name);
    if (g_want_unimpl) CHECK(!strcmp(g_unimpl, name), "%s: answered, not left unimplemented", name);
    else CHECK(!g_unimpl[0], "%s: not implemented (\"%s\")", name, g_unimpl);
    return c->eax;
}
static uint32_t last_error(Thr *t){ return MR32(t->cpu.fs_base + 0x34u); }
static void set_last_error(Thr *t, uint32_t e){ MR32(t->cpu.fs_base + 0x34u) = e; }
static void thr_init(Thr *t, uint32_t teb, uint32_t stack_top, uint32_t tid){
    memset(t, 0, sizeof *t);
    t->stack_top = stack_top;
    t->cpu.fs_base = teb; t->cpu.fcw = 0x027f;
    memset(g_mem + teb, 0, 0x1000);
    MR32(teb + 0x18u) = teb; MR32(teb + 0x24u) = tid;
}
static double now_ms(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }
static uint32_t gstr(uint32_t p, const char *s){ size_t n = strlen(s) + 1; memcpy(g_mem + p, s, n); return p; }
static uint32_t gwstr(uint32_t p, const char *s){ size_t n = strlen(s) + 1; for (size_t i = 0; i < n; i++) MR16(p + 2u * (uint32_t)i) = (uint8_t)s[i]; return p; }
static void put_st(uint32_t p, int y, int mo, int dow, int d, int h, int mi, int s, int ms){
    MR16(p) = (uint16_t)y; MR16(p + 2) = (uint16_t)mo; MR16(p + 4) = (uint16_t)dow; MR16(p + 6) = (uint16_t)d;
    MR16(p + 8) = (uint16_t)h; MR16(p + 10) = (uint16_t)mi; MR16(p + 12) = (uint16_t)s; MR16(p + 14) = (uint16_t)ms;
}
static uint64_t rd64(uint32_t p){ return (uint64_t)MR32(p) | ((uint64_t)MR32(p + 4u) << 32); }
static void wr64(uint32_t p, uint64_t v){ MR32(p) = (uint32_t)v; MR32(p + 4u) = (uint32_t)(v >> 32); }
#define FT_EPOCH 134320464000000000ull     /* 2026-08-24 12:00:00 UTC (a Monday) */
#define INF 0xFFFFFFFFu
#define WAIT_TIMEOUT 0x102u
#define WAIT_FAILED 0xFFFFFFFFu

/* ======================= M2-09: waitable timers ======================= */
static void test_timers(Thr *t){
    const uint32_t st = SCRATCH, ft = SCRATCH + 0x20, due = SCRATCH + 0x30, arr = SCRATCH + 0x40;
    char d[200];
    /* the guest's system time is the fixed date plus the virtual clock */
    atomic_store(&g_vclock_freeze, 5000000ull);                      /* 5 s of virtual time */
    memset(g_mem + st, 0xAA, 16);
    call_imp(t, "GetSystemTime", st);
    CHECK(MR16(st) == 2026 && MR16(st + 2) == 8 && MR16(st + 4) == 1 && MR16(st + 6) == 24 && MR16(st + 8) == 12 &&
          MR16(st + 10) == 0 && MR16(st + 12) == 5 && MR16(st + 14) == 0,
          "GetSystemTime at 5 s: %u-%u-%u (dow %u) %u:%u:%u.%u", MR16(st), MR16(st + 2), MR16(st + 6), MR16(st + 4),
          MR16(st + 8), MR16(st + 10), MR16(st + 12), MR16(st + 14));
    CHECK(call_imp(t, "SystemTimeToFileTime", st, ft) == 1 && rd64(ft) == FT_EPOCH + 50000000ull,
          "SystemTimeToFileTime(GetSystemTime) = %llu", (unsigned long long)rd64(ft));
    /* documented values: 1601-01-01 is 0, 1970-01-01 is 116444736000000000, the last SYSTEMTIME is 0x7FFF35F4F06C58F0 */
    put_st(st, 1601, 1, 1, 1, 0, 0, 0, 0);
    CHECK(call_imp(t, "SystemTimeToFileTime", st, ft) == 1 && rd64(ft) == 0, "1601-01-01 -> %llu", (unsigned long long)rd64(ft));
    put_st(st, 1970, 1, 4, 1, 0, 0, 0, 0);
    CHECK(call_imp(t, "SystemTimeToFileTime", st, ft) == 1 && rd64(ft) == 116444736000000000ull, "1970-01-01 -> %llu", (unsigned long long)rd64(ft));
    put_st(st, 2000, 2, 0, 29, 12, 34, 56, 789);                     /* wDayOfWeek is not looked at */
    CHECK(call_imp(t, "SystemTimeToFileTime", st, ft) == 1 && rd64(ft) == 125963012967890000ull, "2000-02-29 -> %llu", (unsigned long long)rd64(ft));
    put_st(st, 30827, 12, 0, 31, 23, 59, 59, 999);
    CHECK(call_imp(t, "SystemTimeToFileTime", st, ft) == 1 && rd64(ft) == 0x7FFF35F4F06C58F0ull, "30827-12-31 -> 0x%llx", (unsigned long long)rd64(ft));
    static const int bad[][7] = { { 2001, 2, 29, 0, 0, 0, 0 }, { 2026, 13, 1, 0, 0, 0, 0 }, { 2026, 4, 31, 0, 0, 0, 0 },
                                  { 2026, 1, 1, 24, 0, 0, 0 }, { 2026, 1, 1, 0, 60, 0, 0 }, { 2026, 1, 1, 0, 0, 60, 0 },
                                  { 2026, 1, 1, 0, 0, 0, 1000 }, { 1600, 12, 31, 0, 0, 0, 0 }, { 2026, 0, 1, 0, 0, 0, 0 } };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++){
        put_st(st, bad[i][0], bad[i][1], 0, bad[i][2], bad[i][3], bad[i][4], bad[i][5], bad[i][6]);
        set_last_error(t, 0);
        CHECK(call_imp(t, "SystemTimeToFileTime", st, ft) == 0 && last_error(t) == 87, "SystemTimeToFileTime refuses bad date %zu (error %u)", i, last_error(t));
    }

    /* a manual-reset timer re-armed with absolute due times, as MW's 100 Hz thread does */
    uint32_t h = call_imp(t, "CreateWaitableTimerA", 0u, 1u, 0u);
    CHECK(h >= H_TIMER0 && h < H_TIMER0 + 0x1000u, "CreateWaitableTimerA -> 0x%08x", h);
    CHECK(call_imp(t, "WaitForSingleObject", h, 0u) == WAIT_TIMEOUT, "a timer never set is not signaled");
    call_imp(t, "GetSystemTime", st); call_imp(t, "SystemTimeToFileTime", st, ft);
    wr64(due, rd64(ft) + 100000ull);                                 /* now + 10 ms */
    CHECK(call_imp(t, "SetWaitableTimer", h, due, 0u, 0u, 0u, 0u) == 1, "SetWaitableTimer absolute");
    CHECK(call_imp(t, "WaitForSingleObject", h, 0u) == WAIT_TIMEOUT, "not due yet");
    atomic_store(&g_vclock_freeze, 5009999ull);
    CHECK(call_imp(t, "WaitForSingleObject", h, 0u) == WAIT_TIMEOUT, "1 us before the due time");
    atomic_store(&g_vclock_freeze, 5010000ull);
    CHECK(call_imp(t, "WaitForSingleObject", h, 0u) == 0, "at the due time: WAIT_OBJECT_0");
    CHECK(call_imp(t, "WaitForSingleObject", h, 0u) == 0, "manual reset: still signaled");
    CHECK(win32_obj_describe(h, d, sizeof d) > 0 && strstr(d, "timer, manual reset, signaled 1, not armed"), "describe: %s", d);
    wr64(due, rd64(due) + 100000ull);                                /* the thread's next due time: + the period */
    CHECK(call_imp(t, "SetWaitableTimer", h, due, 0u, 0u, 0u, 0u) == 1, "re-armed");
    CHECK(call_imp(t, "WaitForSingleObject", h, 0u) == WAIT_TIMEOUT, "re-arming resets the signaled state");
    CHECK(win32_obj_describe(h, d, sizeof d) > 0 && strstr(d, "due in 10.0 ms"), "describe: %s", d);
    atomic_store(&g_vclock_freeze, 5020000ull);
    CHECK(call_imp(t, "WaitForSingleObject", h, 0u) == 0, "second period");
    /* a due time in the past fires at once: the epoch itself, and FILETIME 0 */
    wr64(due, FT_EPOCH);
    call_imp(t, "SetWaitableTimer", h, due, 0u, 0u, 0u, 0u);
    CHECK(call_imp(t, "WaitForSingleObject", h, 0u) == 0, "a past absolute due time fires at once");
    wr64(due, 0);
    call_imp(t, "SetWaitableTimer", h, due, 0u, 0u, 0u, 0u);
    CHECK(call_imp(t, "WaitForSingleObject", h, 0u) == 0, "FILETIME 0 fires at once");
    /* relative: negative, 100 ns units */
    wr64(due, (uint64_t)-150000ll);                                  /* 15 ms */
    call_imp(t, "SetWaitableTimer", h, due, 0u, 0u, 0u, 0u);
    atomic_store(&g_vclock_freeze, 5034999ull);
    CHECK(call_imp(t, "WaitForSingleObject", h, 0u) == WAIT_TIMEOUT, "relative: 1 us early");
    atomic_store(&g_vclock_freeze, 5035000ull);
    CHECK(call_imp(t, "WaitForSingleObject", h, 0u) == 0, "relative: on time");
    /* CancelWaitableTimer disarms and leaves the signaled state as it is */
    CHECK(call_imp(t, "CancelWaitableTimer", h) == 1 && call_imp(t, "WaitForSingleObject", h, 0u) == 0, "cancel keeps the signaled state");
    wr64(due, (uint64_t)-100000ll);
    call_imp(t, "SetWaitableTimer", h, due, 0u, 0u, 0u, 0u);
    call_imp(t, "CancelWaitableTimer", h);
    atomic_store(&g_vclock_freeze, 5100000ull);
    CHECK(call_imp(t, "WaitForSingleObject", h, 0u) == WAIT_TIMEOUT, "a cancelled timer does not fire");

    /* a periodic auto-reset timer: one waiter per period, missed periods signal once */
    uint32_t a = call_imp(t, "CreateWaitableTimerA", 0u, 0u, gstr(SCRATCH + 0x100, "tick"));
    wr64(due, (uint64_t)-100000ll);                                  /* first at +10 ms, then every 20 ms */
    CHECK(call_imp(t, "SetWaitableTimer", a, due, 20u, 0u, 0u, 0u) == 1, "periodic timer set");
    atomic_store(&g_vclock_freeze, 5110000ull);
    CHECK(call_imp(t, "WaitForSingleObject", a, 0u) == 0, "periodic: first");
    CHECK(call_imp(t, "WaitForSingleObject", a, 0u) == WAIT_TIMEOUT, "auto reset: taken by the wait");
    atomic_store(&g_vclock_freeze, 5129999ull);
    CHECK(call_imp(t, "WaitForSingleObject", a, 0u) == WAIT_TIMEOUT, "periodic: 1 us before the second");
    atomic_store(&g_vclock_freeze, 5130000ull);
    CHECK(call_imp(t, "WaitForSingleObject", a, 0u) == 0, "periodic: second");
    atomic_store(&g_vclock_freeze, 5200000ull);                      /* 3.5 periods later */
    CHECK(call_imp(t, "WaitForSingleObject", a, 0u) == 0, "missed periods: signaled");
    CHECK(call_imp(t, "WaitForSingleObject", a, 0u) == WAIT_TIMEOUT, "missed periods: once");
    atomic_store(&g_vclock_freeze, 5209999ull);
    CHECK(call_imp(t, "WaitForSingleObject", a, 0u) == WAIT_TIMEOUT, "the next due time keeps the phase (5,210,000)");
    atomic_store(&g_vclock_freeze, 5210000ull);
    CHECK(call_imp(t, "WaitForSingleObject", a, 0u) == 0, "periodic: in phase again");
    CHECK(win32_obj_describe(a, d, sizeof d) > 0 && strstr(d, "timer tick, auto reset") && strstr(d, "period 20.0 ms"), "describe: %s", d);

    /* errors */
    set_last_error(t, 0);
    CHECK(call_imp(t, "SetWaitableTimer", 0x1234u, due, 0u, 0u, 0u, 0u) == 0 && last_error(t) == 6, "no such timer: ERROR_INVALID_HANDLE");
    uint32_t ev0 = call_imp(t, "CreateEventA", 0u, 1u, 0u, 0u);
    set_last_error(t, 0);
    CHECK(call_imp(t, "SetWaitableTimer", ev0, due, 0u, 0u, 0u, 0u) == 0 && last_error(t) == 6, "an event is no timer");
    set_last_error(t, 0);
    CHECK(call_imp(t, "SetWaitableTimer", h, due, (uint32_t)-1, 0u, 0u, 0u) == 0 && last_error(t) == 87, "negative period");
    set_last_error(t, 0);
    CHECK(call_imp(t, "SetWaitableTimer", h, 0u, 0u, 0u, 0u, 0u) == 0 && last_error(t) == 87, "no due time");
    set_last_error(t, 0);
    CHECK(call_imp(t, "SetWaitableTimer", h, due, 0u, 0u, 0u, 1u) == 1 && last_error(t) == 50, "fResume: TRUE and ERROR_NOT_SUPPORTED");
    CHECK(call_imp(t, "SetWaitableTimer", h, due, 0u, 0x00401000u, 0u, 0u) == 1, "a completion routine: still set");
    set_last_error(t, 0);
    CHECK(call_imp(t, "CancelWaitableTimer", ev0) == 0 && last_error(t) == 6, "cancel an event");

    /* finite waits in real time while the clock stands still */
    wr64(due, (uint64_t)-10000000ll);                                /* 1 s away */
    call_imp(t, "SetWaitableTimer", h, due, 0u, 0u, 0u, 0u);
    double t0 = now_ms();
    uint32_t r = call_imp(t, "WaitForSingleObject", h, 30u);
    double el = now_ms() - t0;
    CHECK(r == WAIT_TIMEOUT && el >= 25.0 && el < 1000.0, "WaitForSingleObject(30 ms) on a frozen clock: %u after %.1f ms", r, el);

    /* WaitForMultipleObjects with a timer in the set */
    uint32_t ev = call_imp(t, "CreateEventA", 0u, 1u, 0u, 0u);
    wr64(due, (uint64_t)-100000ll);
    call_imp(t, "SetWaitableTimer", h, due, 0u, 0u, 0u, 0u);        /* due at 5,210,000 + 10 ms */
    MR32(arr) = ev; MR32(arr + 4) = h;
    CHECK(call_imp(t, "WaitForMultipleObjects", 2u, arr, 0u, 0u) == WAIT_TIMEOUT, "WFMO: none ready");
    call_imp(t, "SetEvent", ev);
    CHECK(call_imp(t, "WaitForMultipleObjects", 2u, arr, 0u, 0u) == 0, "WFMO: the event, index 0");
    call_imp(t, "ResetEvent", ev);
    atomic_store(&g_vclock_freeze, 5220000ull);
    CHECK(call_imp(t, "WaitForMultipleObjects", 2u, arr, 0u, 0u) == 1, "WFMO: the timer, index 1");
    CHECK(call_imp(t, "WaitForMultipleObjects", 2u, arr, 1u, 0u) == WAIT_TIMEOUT, "WFMO all: the event is not set");
    call_imp(t, "SetEvent", ev);
    CHECK(call_imp(t, "WaitForMultipleObjects", 2u, arr, 1u, 0u) == 0, "WFMO all: both");
    call_imp(t, "SetEvent", ev);
    CHECK(call_imp(t, "WaitForMultipleObjects", 2u, arr, 0u, 0u) == 0, "WFMO any: the lowest index of the ready ones");
    MR32(arr) = a; MR32(arr + 4) = ev;                               /* auto-reset timer, due at 5,230,000 */
    call_imp(t, "ResetEvent", ev);
    atomic_store(&g_vclock_freeze, 5230000ull);
    CHECK(call_imp(t, "WaitForMultipleObjects", 2u, arr, 0u, 0u) == 0, "WFMO: the auto-reset timer");
    CHECK(call_imp(t, "WaitForMultipleObjects", 2u, arr, 0u, 0u) == WAIT_TIMEOUT, "WFMO: took the auto-reset timer's signal");
    MR32(arr) = h; MR32(arr + 4) = h;
    set_last_error(t, 0);
    CHECK(call_imp(t, "WaitForMultipleObjects", 2u, arr, 1u, 0u) == WAIT_FAILED && last_error(t) == 87, "WFMO all: a handle twice");
    MR32(arr) = h; MR32(arr + 4) = 0x1234u;
    set_last_error(t, 0);
    CHECK(call_imp(t, "WaitForMultipleObjects", 2u, arr, 0u, 0u) == WAIT_FAILED && last_error(t) == 6, "WFMO: not a handle");
    MR32(arr) = ev; MR32(arr + 4) = a;
    call_imp(t, "ResetEvent", ev);
    t0 = now_ms();
    r = call_imp(t, "WaitForMultipleObjects", 2u, arr, 0u, 30u);
    el = now_ms() - t0;
    CHECK(r == WAIT_TIMEOUT && el >= 25.0 && el < 1000.0, "WFMO(30 ms) on a frozen clock: %u after %.1f ms", r, el);

    /* the clock running: a wait the timer ends, in real time */
    atomic_store(&g_vclock_freeze, 0ull);
    vclock_tick();
    wr64(due, (uint64_t)-200000ll);                                  /* 20 ms */
    call_imp(t, "SetWaitableTimer", h, due, 0u, 0u, 0u, 0u);
    uint64_t v0 = host_us(); t0 = now_ms();
    r = call_imp(t, "WaitForSingleObject", h, 2000u);
    el = now_ms() - t0; uint64_t v1 = host_us();
    CHECK(r == 0 && v1 - v0 >= 20000u && el < 1000.0, "a 20 ms timer on the running clock: %u after %.1f ms (%.1f ms virtual)", r, el, (double)(v1 - v0) / 1000.0);
    vclock_tick();
    wr64(due, (uint64_t)-150000ll);
    call_imp(t, "SetWaitableTimer", h, due, 0u, 0u, 0u, 0u);
    t0 = now_ms();
    r = call_imp(t, "WaitForSingleObject", h, INF);
    el = now_ms() - t0;
    CHECK(r == 0 && el < 1000.0, "INFINITE on a timer due in 15 ms: %u after %.1f ms", r, el);
    vclock_tick();
    MR32(arr) = ev; MR32(arr + 4) = h;
    call_imp(t, "SetWaitableTimer", h, due, 0u, 0u, 0u, 0u);
    t0 = now_ms();
    r = call_imp(t, "WaitForMultipleObjects", 2u, arr, 0u, INF);
    el = now_ms() - t0;
    CHECK(r == 1 && el < 1000.0, "WFMO INFINITE: the timer after %.1f ms (%u)", el, r);
    call_imp(t, "CloseHandle", h); call_imp(t, "CloseHandle", a); call_imp(t, "CloseHandle", ev); call_imp(t, "CloseHandle", ev0);
}

/* ======================= M2-10: small imports ======================= */
static uint32_t s_date(Thr *t, uint32_t fl, uint32_t st, const char *fmt, char *out){
    uint32_t buf = SCRATCH + 0x800, pf = fmt ? gstr(SCRATCH + 0x700, fmt) : 0u;
    memset(g_mem + buf, 0x55, 128);
    uint32_t r = call_imp(t, "GetDateFormatA", 0x409u, fl, st, pf, buf, 128u);
    snprintf(out, 128, "%s", r ? (const char *)(g_mem + buf) : "");
    return r;
}
static uint32_t s_time(Thr *t, uint32_t fl, uint32_t st, const char *fmt, char *out){
    uint32_t buf = SCRATCH + 0x800, pf = fmt ? gstr(SCRATCH + 0x700, fmt) : 0u;
    memset(g_mem + buf, 0x55, 128);
    uint32_t r = call_imp(t, "GetTimeFormatA", 0x400u, fl, st, pf, buf, 128u);
    snprintf(out, 128, "%s", r ? (const char *)(g_mem + buf) : "");
    return r;
}
static uint32_t cmp_a(Thr *t, uint32_t fl, const char *x, int nx, const char *y, int ny){
    return call_imp(t, "CompareStringA", 0x400u, fl, gstr(SCRATCH + 0x900, x), (uint32_t)nx, gstr(SCRATCH + 0xA00, y), (uint32_t)ny);
}
static uint32_t cmp_w(Thr *t, uint32_t fl, const char *x, int nx, const char *y, int ny){
    return call_imp(t, "CompareStringW", 0x400u, fl, gwstr(SCRATCH + 0x900, x), (uint32_t)nx, gwstr(SCRATCH + 0xB00, y), (uint32_t)ny);
}
static void test_small(Thr *t){
    const uint32_t p = SCRATCH + 0x100, st = SCRATCH + 0x200;
    char s[128];
    /* GlobalMemoryStatusEx: the whole 64-byte MEMORYSTATUSEX, dwLength checked, >= 400 MiB */
    memset(g_mem + p, 0xAA, 64); MR32(p) = 64;
    CHECK(call_imp(t, "GlobalMemoryStatusEx", p) == 1, "GlobalMemoryStatusEx");
    CHECK(rd64(p + 8) >= 400ull << 20 && rd64(p + 16) <= rd64(p + 8) && rd64(p + 24) >= rd64(p + 8) && rd64(p + 32) <= rd64(p + 24) &&
          rd64(p + 40) == 0x7FFE0000ull && rd64(p + 48) <= rd64(p + 40) && rd64(p + 56) == 0 && MR32(p + 4) <= 100,
          "MEMORYSTATUSEX: total %llu, avail %llu, page file %llu/%llu, virtual %llu/%llu, extended %llu, load %u",
          (unsigned long long)rd64(p + 8), (unsigned long long)rd64(p + 16), (unsigned long long)rd64(p + 24),
          (unsigned long long)rd64(p + 32), (unsigned long long)rd64(p + 40), (unsigned long long)rd64(p + 48),
          (unsigned long long)rd64(p + 56), MR32(p + 4));
    { int left = 0; for (uint32_t i = 4; i < 64; i++) left += MR8(p + i) == 0xAA; CHECK(!left, "MEMORYSTATUSEX: %d bytes not written", left); }
    MR32(p) = 32; set_last_error(t, 0);
    CHECK(call_imp(t, "GlobalMemoryStatusEx", p) == 0 && last_error(t) == 87, "dwLength 32: ERROR_INVALID_PARAMETER");

    /* GetDateFormatA, en-US.  The documentation's own example: "ddd',' MMM dd yy" -> "Wed, Aug 31 94" */
    put_st(st, 1994, 8, 0, 31, 23, 29, 40, 0);                       /* wDayOfWeek wrong on purpose: ignored */
    CHECK(s_date(t, 0, st, "ddd',' MMM dd yy", s) == 15 && !strcmp(s, "Wed, Aug 31 94"), "picture: \"%s\"", s);
    CHECK(s_date(t, 0, st, NULL, s) == 10 && !strcmp(s, "8/31/1994"), "short date: \"%s\"", s);
    CHECK(s_date(t, 2, st, NULL, s) && !strcmp(s, "Wednesday, August 31, 1994"), "long date: \"%s\"", s);
    CHECK(s_date(t, 8, st, NULL, s) && !strcmp(s, "August, 1994"), "year and month: \"%s\"", s);
    put_st(st, 2026, 3, 4, 5, 0, 0, 0, 0);
    CHECK(s_date(t, 2, st, NULL, s) && !strcmp(s, "Thursday, March 05, 2026"), "long date, XP's dd: \"%s\"", s);
    CHECK(s_date(t, 0, st, "dddd d MMMM M MM yyyy yy gg", s) && !strcmp(s, "Thursday 5 March 3 03 2026 26 A.D."), "pictures: \"%s\"", s);
    CHECK(s_date(t, 0, st, "'Today is' d'''s' ''x''", s) && !strcmp(s, "Today is 5's 'x'"), "quotes: \"%s\"", s);
    CHECK(call_imp(t, "GetDateFormatA", 0x409u, 0u, st, 0u, 0u, 0u) == 9, "cch 0: the size with the NUL (9)");
    set_last_error(t, 0);
    CHECK(call_imp(t, "GetDateFormatA", 0x409u, 0u, st, 0u, SCRATCH + 0x800, 5u) == 0 && last_error(t) == 122, "too small: ERROR_INSUFFICIENT_BUFFER");
    set_last_error(t, 0);
    CHECK(call_imp(t, "GetDateFormatA", 0x409u, 1u, st, gstr(SCRATCH + 0x700, "d"), SCRATCH + 0x800, 64u) == 0 && last_error(t) == 1004,
          "a picture and DATE_SHORTDATE: ERROR_INVALID_FLAGS");
    set_last_error(t, 0);
    CHECK(call_imp(t, "GetDateFormatA", 0x409u, 3u, st, 0u, SCRATCH + 0x800, 64u) == 0 && last_error(t) == 1004, "short and long: ERROR_INVALID_FLAGS");
    put_st(st, 2026, 2, 0, 29, 0, 0, 0, 0); set_last_error(t, 0);
    CHECK(call_imp(t, "GetDateFormatA", 0x409u, 0u, st, 0u, SCRATCH + 0x800, 64u) == 0 && last_error(t) == 87, "2026-02-29: ERROR_INVALID_PARAMETER");
    CHECK(s_date(t, 0, 0u, NULL, s) && !strcmp(s, "8/24/2026"), "no date: the runtime's local date: \"%s\"", s);

    /* GetTimeFormatA: the documentation's example "hh':'mm':'ss tt" -> "11:29:40 PM" */
    put_st(st, 1994, 8, 3, 31, 23, 29, 40, 0);
    CHECK(s_time(t, 0, st, "hh':'mm':'ss tt", s) == 12 && !strcmp(s, "11:29:40 PM"), "picture: \"%s\"", s);
    CHECK(s_time(t, 0, st, NULL, s) && !strcmp(s, "11:29:40 PM"), "default: \"%s\"", s);
    CHECK(s_time(t, 2, st, NULL, s) && !strcmp(s, "11:29 PM"), "TIME_NOSECONDS: \"%s\"", s);
    CHECK(s_time(t, 1, st, NULL, s) && !strcmp(s, "11 PM"), "TIME_NOMINUTESORSECONDS: \"%s\"", s);
    CHECK(s_time(t, 4, st, NULL, s) && !strcmp(s, "11:29:40"), "TIME_NOTIMEMARKER: \"%s\"", s);
    CHECK(s_time(t, 12, st, NULL, s) && !strcmp(s, "23:29:40"), "24 hours, no marker: \"%s\"", s);
    CHECK(s_time(t, 0, st, "HH'h'mm t", s) && !strcmp(s, "23h29 P"), "HH, t: \"%s\"", s);
    put_st(st, 0, 0, 0, 0, 0, 5, 9, 0);                              /* the date is not looked at */
    CHECK(s_time(t, 0, st, NULL, s) && !strcmp(s, "12:05:09 AM"), "midnight: \"%s\"", s);
    put_st(st, 0, 0, 0, 0, 24, 0, 0, 0); set_last_error(t, 0);
    CHECK(call_imp(t, "GetTimeFormatA", 0x409u, 0u, st, 0u, SCRATCH + 0x800, 64u) == 0 && last_error(t) == 87, "hour 24: ERROR_INVALID_PARAMETER");
    set_last_error(t, 0);
    CHECK(call_imp(t, "GetTimeFormatA", 0x409u, 0x100u, 0u, 0u, SCRATCH + 0x800, 64u) == 0 && last_error(t) == 1004, "unknown flag: ERROR_INVALID_FLAGS");
    CHECK(s_time(t, 0, 0u, NULL, s) && !strcmp(s, "12:00:00 PM"), "no time: the runtime's local time: \"%s\"", s);

    /* CompareStringA/W: 1 less, 2 equal, 3 greater */
    CHECK(cmp_a(t, 0, "abc", -1, "abc", -1) == 2, "abc = abc");
    CHECK(cmp_a(t, 0, "abc", -1, "ABC", -1) == 1, "abc < ABC (lower case first)");
    CHECK(cmp_a(t, 1, "abc", -1, "ABC", -1) == 2, "NORM_IGNORECASE: abc = ABC");
    CHECK(cmp_a(t, 0, "a", -1, "B", -1) == 1, "a < B (alphabet, not ASCII)");
    CHECK(cmp_a(t, 0, "abc", -1, "abcd", -1) == 1, "a prefix is less");
    CHECK(cmp_a(t, 0, "abcX", 3, "abc", -1) == 2, "counted: abc(X) = abc");
    CHECK(cmp_a(t, 0, "!", -1, "0", -1) == 1 && cmp_a(t, 0, "9", -1, "a", -1) == 1, "symbols < digits < letters");
    CHECK(cmp_a(t, 0, "coop", -1, "co-op", -1) == 1, "word sort: coop < co-op (together)");
    CHECK(cmp_a(t, 0, "co-op", -1, "cop", -1) == 1, "word sort: co-op < cop");
    CHECK(cmp_a(t, 0x1000, "co-op", -1, "coop", -1) == 1, "SORT_STRINGSORT: co-op < coop (a symbol)");
    CHECK(cmp_a(t, 4, "a.b c", -1, "abc", -1) == 2, "NORM_IGNORESYMBOLS");
    CHECK(cmp_a(t, 0, "resume", -1, "r\xe9sum\xe9", -1) == 1 && cmp_a(t, 2, "resume", -1, "r\xe9sum\xe9", -1) == 2, "accents, NORM_IGNORENONSPACE");
    CHECK(cmp_a(t, 0, "r\xe9sum\xe9", -1, "resumes", -1) == 1, "accents count after the letters");
    CHECK(cmp_a(t, 0, "\xc6on", -1, "aeon", -1) == 3 && cmp_a(t, 1, "\xc6on", -1, "aeon", -1) == 2, "AE is two letters");
    CHECK(cmp_w(t, 0, "abc", -1, "abd", -1) == 1 && cmp_w(t, 1, "HeLLo", -1, "hello", -1) == 2 && cmp_w(t, 0, "b", -1, "A", -1) == 3, "CompareStringW");
    set_last_error(t, 0);
    CHECK(call_imp(t, "CompareStringA", 0x400u, 0u, 0u, (uint32_t)-1, gstr(SCRATCH + 0xA00, "x"), (uint32_t)-1) == 0 && last_error(t) == 87, "NULL string");
    set_last_error(t, 0);
    CHECK(call_imp(t, "CompareStringA", 0x400u, 0x80u, gstr(SCRATCH + 0x900, "x"), (uint32_t)-1, gstr(SCRATCH + 0xA00, "x"), (uint32_t)-1) == 0 &&
          last_error(t) == 1004, "unknown flag: ERROR_INVALID_FLAGS");

    /* SetStdHandle and what GetStdHandle answers */
    CHECK(call_imp(t, "GetStdHandle", (uint32_t)-10) == 0x12u && call_imp(t, "GetStdHandle", (uint32_t)-11) == 0x11u &&
          call_imp(t, "GetStdHandle", (uint32_t)-12) == 0x10u, "GetStdHandle as before");
    CHECK(call_imp(t, "SetStdHandle", (uint32_t)-11, 0x1234u) == 1 && call_imp(t, "GetStdHandle", (uint32_t)-11) == 0x1234u &&
          call_imp(t, "GetStdHandle", (uint32_t)-10) == 0x12u, "SetStdHandle(STD_OUTPUT_HANDLE)");
    call_imp(t, "SetStdHandle", (uint32_t)-11, 0x11u);
    set_last_error(t, 0);
    CHECK(call_imp(t, "SetStdHandle", 5u, 1u) == 0 && last_error(t) == 6, "SetStdHandle(5): ERROR_INVALID_HANDLE");

    /* SetEnvironmentVariableA (GetEnvironmentVariableA reads it back) */
    uint32_t nm = gstr(SCRATCH + 0x300, "NFSU2TEST"), lnm = gstr(SCRATCH + 0x320, "nfsu2test"), buf = SCRATCH + 0x340;
    CHECK(call_imp(t, "SetEnvironmentVariableA", nm, gstr(SCRATCH + 0x330, "abc")) == 1, "SetEnvironmentVariableA");
    CHECK(call_imp(t, "GetEnvironmentVariableA", lnm, buf, 16u) == 3 && !strcmp((char *)g_mem + buf, "abc"), "read back in another case");
    CHECK(call_imp(t, "GetEnvironmentVariableA", nm, buf, 3u) == 4, "too small: the size with the NUL");
    CHECK(call_imp(t, "SetEnvironmentVariableA", nm, 0u) == 1, "NULL deletes");
    set_last_error(t, 0);
    CHECK(call_imp(t, "GetEnvironmentVariableA", nm, buf, 16u) == 0 && last_error(t) == 203, "deleted: ERROR_ENVVAR_NOT_FOUND");
    CHECK(call_imp(t, "SetEnvironmentVariableA", nm, 0u) == 1, "deleting a missing one succeeds");
    set_last_error(t, 0);
    CHECK(call_imp(t, "SetEnvironmentVariableA", gstr(SCRATCH + 0x300, "A=B"), gstr(SCRATCH + 0x330, "x")) == 0 && last_error(t) == 87, "'=' in the name");
    set_last_error(t, 0);
    CHECK(call_imp(t, "SetEnvironmentVariableA", gstr(SCRATCH + 0x300, ""), gstr(SCRATCH + 0x330, "x")) == 0 && last_error(t) == 87, "empty name");

    /* the keyboard: the US layout */
    CHECK(call_imp(t, "GetKeyboardLayout", 0u) == 0x04090409u, "GetKeyboardLayout");
    static const struct { uint32_t code, type, want; } mv[] = {
        { 'A', 0, 0x1E }, { 0x1E, 1, 'A' }, { 'Q', 0, 0x10 }, { '1', 0, 0x02 }, { 0x0B, 1, '0' }, { 0x2A, 1, 0x10 }, { 0x2A, 3, 0xA0 },
        { 0x36, 3, 0xA1 }, { 0x1D, 3, 0xA2 }, { 0xE01D, 3, 0xA3 }, { 0x38, 3, 0xA4 }, { 0xE038, 3, 0xA5 }, { 0xE038, 1, 0x12 },
        { 0x25, 0, 0x4B }, { 0x25, 4, 0xE04B }, { 0xE04B, 3, 0x25 }, { 0x4B, 3, 0x25 }, { 0x10, 0, 0x2A }, { 0xA1, 0, 0x36 },
        { 'A', 2, 'A' }, { 0xBA, 2, ';' }, { 0xDE, 2, '\'' }, { 0x0D, 2, 0x0D }, { 0x6A, 2, '*' }, { 0x37, 3, 0x6A },
        { 0x4A, 1, 0x6D }, { 0x4E, 1, 0x6B }, { 0xE035, 3, 0x6F }, { 0x1C, 1, 0x0D }, { 0x39, 1, 0x20 }, { 0x3B, 1, 0x70 },
        { 0x57, 1, 0x7A }, { 0xC8, 3, 0 }, { 0x13, 0, 0x45 }, { 0xE11D, 3, 0x13 }, { 0x70, 2, 0 }, { 0x100, 0, 0 },
    };
    for (size_t i = 0; i < sizeof mv / sizeof *mv; i++){
        uint32_t r = call_imp(t, "MapVirtualKeyA", mv[i].code, mv[i].type);
        CHECK(r == mv[i].want, "MapVirtualKeyA(0x%x, %u) = 0x%x, want 0x%x", mv[i].code, mv[i].type, r, mv[i].want);
        r = call_imp(t, "MapVirtualKeyExA", mv[i].code, mv[i].type, 0x04090409u);
        CHECK(r == mv[i].want, "MapVirtualKeyExA(0x%x, %u) = 0x%x, want 0x%x", mv[i].code, mv[i].type, r, mv[i].want);
    }
    uint32_t ks = SCRATCH + 0x400, wb = SCRATCH + 0x500;
    static const struct { uint32_t vk, sc; uint8_t shift, caps, ctrl, alt; uint16_t ch; } tu[] = {
        { 'A', 0x1E, 0, 0, 0, 0, 'a' }, { 'A', 0x1E, 1, 0, 0, 0, 'A' }, { 'A', 0x1E, 0, 1, 0, 0, 'A' }, { 'A', 0x1E, 1, 1, 0, 0, 'a' },
        { 'A', 0x1E, 0, 0, 0, 1, 'a' }, { 'C', 0x2E, 0, 0, 1, 0, 0x03 }, { 'C', 0x2E, 0, 0, 1, 1, 0 }, { '1', 0x02, 0, 0, 0, 0, '1' },
        { '1', 0x02, 1, 0, 0, 0, '!' }, { '1', 0x02, 1, 1, 0, 0, '!' }, { 0xBF, 0x35, 1, 0, 0, 0, '?' }, { 0xDB, 0x1A, 0, 0, 1, 0, 0x1B },
        { 0x20, 0x39, 0, 0, 0, 0, ' ' }, { 0x0D, 0x1C, 0, 0, 0, 0, '\r' }, { 0x08, 0x0E, 0, 0, 0, 0, 0x08 }, { 0x63, 0x51, 0, 0, 0, 0, '3' },
        { 0x70, 0x3B, 0, 0, 0, 0, 0 }, { 0x25, 0x4B, 0, 0, 0, 0, 0 }, { 'A', 0x801E, 0, 0, 0, 0, 0 },
    };
    for (size_t i = 0; i < sizeof tu / sizeof *tu; i++){
        memset(g_mem + ks, 0, 256);
        if (tu[i].shift) MR8(ks + 0x10) = 0x80;
        if (tu[i].caps) MR8(ks + 0x14) = 0x01;
        if (tu[i].ctrl) MR8(ks + 0x11) = 0x80;
        if (tu[i].alt) MR8(ks + 0x12) = 0x80;
        MR32(wb) = 0xEEEEEEEEu;
        uint32_t r = call_imp(t, "ToUnicode", tu[i].vk, tu[i].sc, ks, wb, 4u, 0u);
        CHECK(r == (tu[i].ch ? 1u : 0u) && (!tu[i].ch || (MR16(wb) == tu[i].ch && MR16(wb + 2) == 0)),
              "ToUnicode(vk 0x%x, sc 0x%x, shift %u caps %u ctrl %u alt %u) = %u, U+%04X, want U+%04X", tu[i].vk, tu[i].sc,
              tu[i].shift, tu[i].caps, tu[i].ctrl, tu[i].alt, r, MR16(wb), tu[i].ch);
    }
    /* GetKeyState from the runtime's key state (DirectInput scan codes) */
    CHECK(call_imp(t, "GetKeyState", 0x10u) == 0, "shift up");
    kbd_set_key(KSRC_SCRIPT, 0x2A, 1);
    uint32_t k1 = call_imp(t, "GetKeyState", 0x10u), k2 = call_imp(t, "GetKeyState", 0xA0u), k3 = call_imp(t, "GetKeyState", 0xA1u);
    CHECK((k1 & 0x8000u) && (int32_t)k1 < 0 && (k2 & 0x8000u) && !(k3 & 0x8000u), "left shift down: 0x%x 0x%x 0x%x", k1, k2, k3);
    kbd_set_key(KSRC_SCRIPT, 0x2A, 0);
    CHECK(!(call_imp(t, "GetKeyState", 0x10u) & 0x8000u), "shift up again");
    kbd_set_key(KSRC_SCRIPT, 0x3A, 1);
    uint32_t c1 = call_imp(t, "GetKeyState", 0x14u);
    kbd_set_key(KSRC_SCRIPT, 0x3A, 0);
    uint32_t c2 = call_imp(t, "GetKeyState", 0x14u);
    kbd_set_key(KSRC_SCRIPT, 0x3A, 1); call_imp(t, "GetKeyState", 0x14u); kbd_set_key(KSRC_SCRIPT, 0x3A, 0);
    uint32_t c3 = call_imp(t, "GetKeyState", 0x14u);
    CHECK((c1 & 0x8001u) == 0x8001u && c2 == 1u && c3 == 0u, "caps lock toggles: 0x%x 0x%x 0x%x", c1, c2, c3);
    kbd_set_key(KSRC_SCRIPT, 0xCB, 1);                               /* DIK_LEFT: an extended key */
    CHECK(call_imp(t, "GetKeyState", 0x25u) & 0x8000u, "left arrow down");
    kbd_set_key(KSRC_SCRIPT, 0xCB, 0);
    kbd_set_key(KSRC_SCRIPT, 0x9D, 1);                               /* DIK_RCONTROL */
    CHECK((call_imp(t, "GetKeyState", 0x11u) & 0x8000u) && (call_imp(t, "GetKeyState", 0xA3u) & 0x8000u) &&
          !(call_imp(t, "GetKeyState", 0xA2u) & 0x8000u), "right control: VK_CONTROL and VK_RCONTROL");
    kbd_set_key(KSRC_SCRIPT, 0x9D, 0);

    /* SendInput: the count, nothing injected */
    memset(g_mem + p, 0, 56);                                        /* two INPUTs: a zero mouse move, as MW's pump sends */
    CHECK(call_imp(t, "SendInput", 2u, p, 28u) == 2, "SendInput returns the count");
    set_last_error(t, 0);
    CHECK(call_imp(t, "SendInput", 1u, p, 24u) == 0 && last_error(t) == 87, "SendInput with a wrong cbSize");

    /* refused, logged */
    set_last_error(t, 0);
    CHECK(call_imp(t, "QueueUserAPC", 0x00401000u, 0xFFFFFFFEu, 0u) == 0 && last_error(t) == 50, "QueueUserAPC refused");
    set_last_error(t, 0);
    CHECK(call_imp(t, "CreateProcessA", gstr(SCRATCH + 0x600, "C:\\x.exe"), 0u, 0u, 0u, 0u, 0u, 0u, 0u, SCRATCH + 0x640, SCRATCH + 0x680) == 0 &&
          last_error(t) == 50, "CreateProcessA refused");
}

/* ======================= M2-11: the voice devices ======================= */
static void test_voice(Thr *t){
    const uint32_t ph = SCRATCH + 0x100, caps = SCRATCH + 0x200, wfx = SCRATCH + 0x300;
    CHECK(call_imp(t, "waveOutGetNumDevs") == 0 && call_imp(t, "waveInGetNumDevs") == 0, "no wave devices");
    CHECK(call_imp(t, "waveOutGetDevCapsA", 0u, caps, 52u) == 2 && call_imp(t, "waveInGetDevCapsA", 0u, caps, 48u) == 2, "GetDevCaps(0): MMSYSERR_BADDEVICEID");
    CHECK(call_imp(t, "waveOutGetDevCapsA", 0xFFFFFFFFu, caps, 52u) == 6 && call_imp(t, "waveInGetDevCapsA", 0xFFFFFFFFu, caps, 48u) == 6,
          "GetDevCaps(WAVE_MAPPER): MMSYSERR_NODRIVER");
    MR32(ph) = 0xDEADu;
    CHECK(call_imp(t, "waveOutOpen", ph, 0xFFFFFFFFu, wfx, 0u, 0u, 0u) == 6 && MR32(ph) == 0, "waveOutOpen(WAVE_MAPPER): MMSYSERR_NODRIVER, no handle");
    MR32(ph) = 0xDEADu;
    CHECK(call_imp(t, "waveInOpen", ph, 0u, wfx, 0u, 0u, 0x30000u) == 2 && MR32(ph) == 0, "waveInOpen(0): MMSYSERR_BADDEVICEID");
    CHECK(call_imp(t, "waveInOpen", ph, 0xFFFFFFFFu, wfx, 0u, 0u, 0u) == 6, "waveInOpen(WAVE_MAPPER)");
    static const char *const h1[] = { "waveOutReset", "waveOutClose", "waveInStart", "waveInStop", "waveInReset", "waveInClose" };
    for (size_t i = 0; i < sizeof h1 / sizeof *h1; i++) CHECK(call_imp(t, h1[i], 0x1234u) == 5, "%s: MMSYSERR_INVALHANDLE", h1[i]);
    static const char *const h3[] = { "waveOutWrite", "waveOutGetPosition", "waveOutPrepareHeader", "waveOutUnprepareHeader",
                                      "waveInAddBuffer", "waveInGetPosition", "waveInPrepareHeader", "waveInUnprepareHeader" };
    for (size_t i = 0; i < sizeof h3 / sizeof *h3; i++) CHECK(call_imp(t, h3[i], 0x1234u, caps, 32u) == 5, "%s: MMSYSERR_INVALHANDLE", h3[i]);
    CHECK(call_imp(t, "waveOutSetVolume", 0u, 0xFFFFFFFFu) == 6, "waveOutSetVolume: MMSYSERR_NODRIVER");
    MR32(ph) = 0xDEADu;
    CHECK(call_imp(t, "#6", 0u, ph, 0u) == 0x88780078u && MR32(ph) == 0, "DirectSoundCaptureCreate: DSERR_NODRIVER, NULL");
}

/* FatalAppExitA ends the process with STATUS_FATAL_APP_EXIT (0x40000015) */
static void test_fatal(Thr *t){
    g_exit_armed = 1; g_exit_code = 0;
    volatile int came_back = 0;
    if (!setjmp(g_exit_jmp)){
        CPU *c = &t->cpu; c->esp = t->stack_top - 0x100u;
        PUSH32(gstr(SCRATCH + 0x100, "the end")); PUSH32(0u); PUSH32(RET_ADDR);
        win32_call(c, idx_of("FatalAppExitA"));
        came_back = 1;
    }
    g_exit_armed = 0;
    CHECK(!came_back && g_exit_code == 0x40000015, "FatalAppExitA: exit code 0x%x, %s", (unsigned)g_exit_code, came_back ? "returned" : "exited");
}

/* ======================= stress: the waits with threads ======================= */
static _Atomic int g_frames_stop;
static void *frame_thread(void *u){                    /* the game's Present: the virtual clock's frames */
    (void)u;
    while (!atomic_load(&g_frames_stop)){ vclock_tick(); usleep(16667); }
    return 0;
}
#define NT 8
#define TICKS 1000
/* A read of the virtual clock while vclock_tick moves it (win32.c: the frame base and the real base are two
 * stores) can be one frame step off either way, and vclock_tick caps a step at 50 ms */
#define CLOCK_SLACK_US 50000ull
typedef struct { Thr t; int id; uint32_t h, h2, peer; _Atomic int done; int ticks, early, bad, wakes; } Job;
static Job g_job[NT + 2];
static uint32_t job_stack(int i){ return STACK_TOP - 0x20000u * (uint32_t)(i + 1); }
static uint32_t job_teb(int i){ return TEB_MAIN + 0x1000u * (uint32_t)(i + 1); }
/* MW's 100 Hz thread at 1 kHz: a manual-reset timer, absolute due times, WaitForSingleObject(INFINITE) */
static void *tick_thread(void *u){
    Job *j = u; Thr *t = &j->t;
    guest_lock(&t->cpu);
    const uint32_t st = SCRATCH + 0x1000u + 0x100u * (uint32_t)j->id, due = st + 0x20u;
    uint32_t h = call_imp(t, "CreateWaitableTimerA", 0u, 1u, 0u);
    call_imp(t, "GetSystemTime", st); call_imp(t, "SystemTimeToFileTime", st, due);
    wr64(due, rd64(due) + 10000ull);                                  /* 1 ms period */
    call_imp(t, "SetWaitableTimer", h, due, 0u, 0u, 0u, 0u);
    for (int k = 0; k < TICKS; k++){
        uint32_t r = call_imp(t, "WaitForSingleObject", h, INF);
        uint64_t now = FT_EPOCH + host_us() * 10ull;
        if (r != 0) j->bad++;
        /* signaled before its due time, by more than a read of the clock can be off */
        if (now + CLOCK_SLACK_US * 10ull < rd64(due)) j->early++;
        j->ticks++;
        wr64(due, rd64(due) + 10000ull);
        call_imp(t, "SetWaitableTimer", h, due, 0u, 0u, 0u, 0u);
    }
    call_imp(t, "CloseHandle", h);
    guest_unlock();
    atomic_store(&j->done, 1);
    return 0;
}
/* waiters on one periodic auto-reset timer: each period lets one through */
static uint32_t g_shared; static _Atomic int g_shared_stop;
static void *shared_waiter(void *u){
    Job *j = u; Thr *t = &j->t;
    guest_lock(&t->cpu);
    while (!atomic_load(&g_shared_stop)){
        uint32_t r = call_imp(t, "WaitForSingleObject", g_shared, 100u);
        if (r == 0) j->wakes++; else if (r != WAIT_TIMEOUT) j->bad++;
    }
    guest_unlock();
    atomic_store(&j->done, 1);
    return 0;
}
/* a ping-pong through WaitForMultipleObjects sets that hold an unarmed timer */
#define PINGS 300
static void *ping_thread(void *u){
    Job *j = u; Thr *t = &j->t;
    const uint32_t arr = SCRATCH + 0x2000u + 0x10u * (uint32_t)j->id;
    guest_lock(&t->cpu);
    MR32(arr) = j->h; MR32(arr + 4) = j->h2;
    for (int k = 0; k < PINGS; k++){
        if (j->id == NT) call_imp(t, "SetEvent", j->peer);
        uint32_t r = call_imp(t, "WaitForMultipleObjects", 2u, arr, 0u, 2000u);
        if (r != 0) j->bad++;
        else j->wakes++;
        if (j->id != NT) call_imp(t, "SetEvent", j->peer);
    }
    guest_unlock();
    atomic_store(&j->done, 1);
    return 0;
}
/* a waiter whose timer is closed under it */
static void *close_waiter(void *u){
    Job *j = u; Thr *t = &j->t;
    guest_lock(&t->cpu);
    uint32_t r = call_imp(t, "WaitForSingleObject", j->h, INF);
    j->wakes = (int)r; j->bad = (int)last_error(t);
    guest_unlock();
    atomic_store(&j->done, 1);
    return 0;
}
static int wait_jobs(int from, int to, double ms){
    double t0 = now_ms();
    for (;;){
        int all = 1;
        for (int i = from; i < to; i++) all &= atomic_load(&g_job[i].done);
        if (all) return 1;
        if (now_ms() - t0 > ms) return 0;
        usleep(5000);
    }
}
static int start_job(int i, void *(*fn)(void *)){
    Job *j = &g_job[i];
    thr_init(&j->t, job_teb(i), job_stack(i), 200u + (uint32_t)i);
    j->id = i; atomic_store(&j->done, 0); j->ticks = j->early = j->bad = j->wakes = 0;
    pthread_t th;
    if (pthread_create(&th, 0, fn, j)){ printf("FAIL: pthread_create\n"); fail++; return 0; }
    pthread_detach(th);
    return 1;
}
static void test_stress(Thr *t){
    pthread_t fr;
    atomic_store(&g_vclock_freeze, 0ull);
    /* the clock from a frame on, as the game's after its first Present: before any frame host_us reads up to
     * 50 ms (no frame base yet) and goes back to one frame's step at the first */
    vclock_tick();
    if (pthread_create(&fr, 0, frame_thread, 0)){ printf("FAIL: no threads here (run it in the browser host)\n"); fail++; return; }
    guest_unlock();                                                   /* the main thread lets the others run */

    /* 1. 8 threads x 1,000 ticks of a 1 ms timer re-armed with absolute due times */
    double t0 = now_ms(); uint64_t v0 = host_us();
    for (int i = 0; i < NT; i++) start_job(i, tick_thread);
    int ok = wait_jobs(0, NT, 30000.0);
    double el = now_ms() - t0; uint64_t v1 = host_us();
    CHECK(ok, "tick threads finished within 30 s (%.0f ms)", el);
    int ticks = 0, early = 0, bad = 0;
    for (int i = 0; i < NT; i++){ ticks += g_job[i].ticks; early += g_job[i].early; bad += g_job[i].bad; }
    CHECK(ticks == NT * TICKS && !early && !bad, "%d threads: %d ticks (want %d), %d early, %d bad waits", NT, ticks, NT * TICKS, early, bad);
    CHECK(v1 - v0 + 2u * CLOCK_SLACK_US >= TICKS * 1000ull, "the %d ticks took %.0f ms of virtual time (at least %d, less two reads' slack)",
          TICKS, (double)(v1 - v0) / 1000.0, TICKS);
    printf("  tick threads: %d x %d ticks in %.0f ms real, %.0f ms virtual\n", NT, TICKS, el, (double)(v1 - v0) / 1000.0);

    /* 2. 4 waiters on one 2 ms periodic auto-reset timer for 1 s: each period lets one through, so the wakes are
     * at most the periods from the set to the last waiter's end (the waiters in a wait when the stop comes take
     * a period or two more) */
    guest_lock(&t->cpu);
    g_shared = call_imp(t, "CreateWaitableTimerA", 0u, 0u, 0u);
    wr64(SCRATCH + 0x30u, (uint64_t)-20000ll);
    v0 = host_us();
    call_imp(t, "SetWaitableTimer", g_shared, SCRATCH + 0x30u, 2u, 0u, 0u, 0u);
    guest_unlock();
    atomic_store(&g_shared_stop, 0);
    for (int i = 0; i < 4; i++) start_job(i, shared_waiter);
    usleep(1000000);
    atomic_store(&g_shared_stop, 1);
    ok = wait_jobs(0, 4, 5000.0);
    v1 = host_us();
    int wakes = 0; bad = 0;
    for (int i = 0; i < 4; i++){ wakes += g_job[i].wakes; bad += g_job[i].bad; }
    double periods = (double)(v1 - v0) / 2000.0, most = (double)(v1 - v0 + CLOCK_SLACK_US) / 2000.0;
    CHECK(ok && !bad && wakes <= most && wakes >= periods / 4.0, "shared timer: %d wakes in %.0f periods (%s, %d bad)",
          wakes, periods, ok ? "all ended" : "STUCK", bad);
    printf("  shared timer: 4 waiters, %d wakes in %.0f periods of 2 ms\n", wakes, periods);

    /* 3. WaitForMultipleObjects ping-pong, sets with an unarmed timer: a lost wake-up is a 2 s timeout */
    guest_lock(&t->cpu);
    uint32_t e1 = call_imp(t, "CreateEventA", 0u, 0u, 0u, 0u), e2 = call_imp(t, "CreateEventA", 0u, 0u, 0u, 0u);
    uint32_t tm = call_imp(t, "CreateWaitableTimerA", 0u, 1u, 0u);
    guest_unlock();
    g_job[NT].h = e2; g_job[NT].h2 = tm; g_job[NT].peer = e1;
    g_job[NT + 1].h = e1; g_job[NT + 1].h2 = tm; g_job[NT + 1].peer = e2;
    t0 = now_ms();
    start_job(NT + 1, ping_thread); start_job(NT, ping_thread);
    ok = wait_jobs(NT, NT + 2, 60000.0);
    el = now_ms() - t0;
    CHECK(ok && !g_job[NT].bad && !g_job[NT + 1].bad && g_job[NT].wakes == PINGS && g_job[NT + 1].wakes == PINGS,
          "WFMO ping-pong: %d and %d of %d, %d and %d lost, %.0f ms", g_job[NT].wakes, g_job[NT + 1].wakes, PINGS,
          g_job[NT].bad, g_job[NT + 1].bad, el);
    printf("  WFMO ping-pong: %d round trips in %.0f ms\n", PINGS, el);

    /* 4. a handle closed during an INFINITE wait: WAIT_FAILED, ERROR_INVALID_HANDLE */
    guest_lock(&t->cpu);
    uint32_t tc = call_imp(t, "CreateWaitableTimerA", 0u, 1u, 0u);
    guest_unlock();
    g_job[0].h = tc;
    start_job(0, close_waiter);
    usleep(50000);
    guest_lock(&t->cpu); call_imp(t, "CloseHandle", tc); guest_unlock();
    ok = wait_jobs(0, 1, 5000.0);
    CHECK(ok && (uint32_t)g_job[0].wakes == WAIT_FAILED && g_job[0].bad == 6, "closed mid-wait: 0x%x, error %d (%s)",
          (uint32_t)g_job[0].wakes, g_job[0].bad, ok ? "returned" : "STUCK");

    atomic_store(&g_frames_stop, 1);
    pthread_join(fr, 0);
    guest_lock(&t->cpu);
    call_imp(t, "CloseHandle", e1); call_imp(t, "CloseHandle", e2); call_imp(t, "CloseHandle", tm); call_imp(t, "CloseHandle", g_shared);
}

/* UG2's game table (-DWIN32_UNIT_UG2): no g_game.win32_misc, so win32_misc.c answers nothing.  The four of its
 * imports SPEED2.EXE and server.dll have (CompareStringA/W, SetStdHandle, SetEnvironmentVariableA) take win32.c's
 * not-implemented path as they always did: 0 back, the last error as it was, the stack balanced.  GetStdHandle
 * answers as it always did, and a WaitForMultipleObjects over events (no timer made) as before. */
static void test_ug2(Thr *t){
    set_last_error(t, 0x55u);
    g_want_unimpl = 1;
    CHECK(cmp_a(t, 0, "abc", -1, "abc", -1) == 0, "CompareStringA: 0");
    CHECK(cmp_w(t, 0, "abc", -1, "abc", -1) == 0, "CompareStringW: 0");
    CHECK(call_imp(t, "SetStdHandle", (uint32_t)-11, 0x1234u) == 0, "SetStdHandle: 0");
    CHECK(call_imp(t, "SetEnvironmentVariableA", gstr(SCRATCH + 0x300, "NFSU2TEST"), gstr(SCRATCH + 0x330, "abc")) == 0,
          "SetEnvironmentVariableA: 0");
    /* the layer's others, none of them UG2's imports: the same */
    CHECK(call_imp(t, "GlobalMemoryStatusEx", SCRATCH + 0x100) == 0 && call_imp(t, "GetKeyboardLayout", 0u) == 0 &&
          call_imp(t, "GetEnvironmentVariableA", gstr(SCRATCH + 0x300, "NFSU2TEST"), SCRATCH + 0x340, 16u) == 0,
          "GlobalMemoryStatusEx, GetKeyboardLayout, GetEnvironmentVariableA: 0");
    g_want_unimpl = 0;
    CHECK(last_error(t) == 0x55u, "the last error untouched (%u)", last_error(t));
    CHECK(call_imp(t, "GetStdHandle", (uint32_t)-10) == 0x12u && call_imp(t, "GetStdHandle", (uint32_t)-11) == 0x11u &&
          call_imp(t, "GetStdHandle", (uint32_t)-12) == 0x10u, "GetStdHandle as always");
    uint32_t e1 = call_imp(t, "CreateEventA", 0u, 0u, 0u, 0u), e2 = call_imp(t, "CreateEventA", 0u, 0u, 0u, 0u), arr = SCRATCH + 0x400;
    MR32(arr) = e1; MR32(arr + 4u) = e2;
    call_imp(t, "SetEvent", e2);
    CHECK(call_imp(t, "WaitForMultipleObjects", 2u, arr, 0u, 0u) == 1u, "WaitForMultipleObjects: the second event");
    CHECK(call_imp(t, "WaitForMultipleObjects", 2u, arr, 0u, 0u) == WAIT_TIMEOUT, "WaitForMultipleObjects: auto reset");
    call_imp(t, "CloseHandle", e1); call_imp(t, "CloseHandle", e2);
}

int main(int argc, char **argv){
#ifndef WIN32_UNIT_UG2
    const char *mode = argc > 1 ? argv[1] : "hle";
#else
    const char *mode = argc > 1 ? argv[1] : "ug2";
#endif
    memset(g_cbits, 0xFF, 64u << 20 >> 14 >> 3);                      /* the first 64 MB are committed */
    win32_init();
    Thr t; thr_init(&t, TEB_MAIN, STACK_TOP, 100u);
    guest_lock(&t.cpu);
#ifndef WIN32_UNIT_UG2
    if (!strcmp(mode, "hle")){
        test_timers(&t);
        test_small(&t);
        test_voice(&t);
        test_fatal(&t);
    } else if (!strcmp(mode, "stress")) test_stress(&t);
    else { printf("usage: win32_unit.wasm hle|stress (win32_unit_ug2.wasm: ug2)\n"); return 2; }
#else
    if (!strcmp(mode, "ug2")) test_ug2(&t);
    else { printf("usage: win32_unit_ug2.wasm ug2 (win32_unit.wasm: hle|stress)\n"); return 2; }
#endif
    guest_unlock();
    printf("win32_unit: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
