/* The runtime's parts outside the files a wasm unit program links, stubbed:
 * win32_unit.c links runtime/win32.c, win32_ext.c, win32_misc.c and
 * keyboard.c; regpath_unit.c those and fileio.c with a game's table.  Include
 * it after cpu.h, game.h, kbd.h and module.h.
 *   RT_STUBS_FILEIO_REAL   fileio.c is linked: no stand-ins for it, but for
 *                          what it calls of UG2's garage.c
 *   RT_STUBS_LANGUAGE     what language_registry() gives (UG2's language.c:
 *                          "English" by default); 0 without it
 *   RT_STUBS_MODULE_REAL   module.c and modheap.c are linked: no stand-ins for them
 *   RT_STUBS_DISPATCH_OWN  the program has its own dispatch_indirect and call_guest
 *   RT_STUBS_WINSOCK_REAL  winsock.c is linked (wsaov_unit.c): no stand-ins for it
 * g_unimpl names the import win32.c last left unimplemented. */
#ifndef RT_STUBS_H
#define RT_STUBS_H
#include <stdio.h>
#include <string.h>
#include <time.h>
CPU *g_cur_cpu;
void mem_commit(uint32_t a, uint32_t n){ (void)a; (void)n; }
void mem_altstack_install(void){ }
uint32_t g_image_base = 0x00400000u, g_image_size = 0x00200000u;
int memmap_check(uint32_t lo, uint32_t hi, int fatal){ (void)lo; (void)hi; (void)fatal; return 0; }
void guest_backtrace(CPU *c, int n){ (void)c; (void)n; }
#ifndef RT_STUBS_DISPATCH_OWN
int g_guest_exited;
void dispatch_indirect(CPU *c, uint32_t a){ (void)c; (void)a; }
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args){ (void)c; (void)fn; (void)nargs; (void)args; return 0; }
#endif
/* dispatch.c's patch table (a game's install; MW's d3dxmath_mw.c): taken, none applied in these units */
void dispatch_patch(uint32_t a, void (*f)(CPU *)){ (void)a; (void)f; }
int dispatch_patch_take(uint32_t a, void (*f)(CPU *)){ (void)a; (void)f; return 1; }
void heap_init(void){ }
void heap_report(void){ }
int heap_api_kind(const char *name){ (void)name; return 0; }
uint32_t heap_api(CPU *c, int kind){ (void)c; (void)kind; return 0; }
static uint32_t g_bump = 0x00300000u;
uint32_t gheap_alloc(int heap, uint32_t n, int zero){ (void)heap; uint32_t p = g_bump; g_bump += (n + 15u) & ~15u; if (zero) memset(g_mem + p, 0, n); return p; }
uint32_t gheap_usize(uint32_t p){ (void)p; return 0; }
void heap_totals(uint64_t *live, uint64_t *peak, uint64_t *va, uint64_t *held){ if (live) *live = 0; if (peak) *peak = 0; if (va) *va = 0; if (held) *held = 0; }
#ifndef RT_STUBS_MODULE_REAL
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
#else
uint32_t com_scratch(uint32_t n){ (void)n; return 0; }
#ifndef RT_STUBS_FILEIO_REAL
int fileio_resolve_game(const char *win, char *out, size_t n){ (void)win; (void)out; (void)n; return 0; }
#endif
#endif
#ifndef RT_STUBS_WINSOCK_REAL
int winsock_call(CPU *c, const char *dll, const char *n, uint32_t *r){ (void)c; (void)dll; (void)n; (void)r; return 0; }
void winsock_report(void){ }
#endif
#ifndef RT_STUBS_FILEIO_REAL
int fileio_call(CPU *c, const char *n, uint32_t *r){ (void)c; (void)n; (void)r; return 0; }
int fileio_is_handle(uint32_t h){ (void)h; return 0; }
void fileio_report(void){ }
#else
void garage_profile_written(const char *path){ (void)path; }   /* garage.c: not linked */
void garage_profile_deleted(const char *path){ (void)path; }
#endif
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
#ifdef RT_STUBS_LANGUAGE
const char *language_registry(void){ return RT_STUBS_LANGUAGE; }
#else
const char *language_registry(void){ return 0; }
#endif
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
void autotest_note_screen(const char *nm){ (void)nm; }          /* runtime/mw/fe_mw.c's screens */
#endif
