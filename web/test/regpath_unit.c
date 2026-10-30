/* The registry and the paths a game sees (TRACKER M2-12, M2-13): Most
 * Wanted's, and UG2's, which stay as they always were.  runtime/win32.c,
 * win32_ext.c, win32_misc.c, keyboard.c and fileio.c are the real ones, and
 * so is the game's table (runtime/mw/game_mw.c with registry_mw.c, or
 * runtime/game_ug2.c with -DREGPATH_UG2); the rest of the runtime is
 * web/test/rt_stubs.h.  Every import is called as lifted code calls it:
 * arguments pushed right to left, a return address, then win32_call with the
 * import's index; after each call the stack pointer is back above the
 * arguments (the table's stdcall count, which web/test/regpath-wasm.test.mjs
 * checks against argcount's KNOWN and the lifts) and the caller's frame is
 * untouched.
 *
 *   regpath_mw.wasm
 *     boot1    a fresh registry: speed.exe's registry calls of a first start, in its order and with
 *              its arguments: ~MHz (0x7EC7A2), the start-up reads (0x6CB680), FirstTime not
 *              there and written, then SAVE_SETTINGS (0x6C1F10) after the GPU auto-detect
 *     boot2    the same registry file: a later start (FirstTime there, LOAD_SETTINGS 0x6C1C00)
 *     keys     the key table's rules: keys it lacks, access rights, handles, types, sizes
 *     home     no NFSU2_REGFILE: the file is $HOME/.nfsmw_registry
 *     paths    C:\NFSMW, the exe's names, SHGetFolderPathA, a save in the overlay, the bin.dat probe
 *              on every drive letter, escapes (drives, '..', UNC, devices, streams)
 *     ro       no save overlay: the game folder is never written
 *     scan     folder scans in NTFS order whatever order the host lists (game.h find_sorted), a deleted
 *              folder gone from the next scan, the game folder and the overlay in one order
 *   regpath_ug2.wasm (-DREGPATH_UG2)
 *     ug2      the flat registry and the paths as UG2 always had them
 *     ug2home  no NFSU2_REGFILE: the file is $HOME/.nfsu2_registry
 *     ug2nosave no save overlay: UG2 writes into its game folder, as it always could
 *
 * The test's folder is the WASI preopen /t: /t/game the game folder (with
 * bin.dat and GLOBAL/GLOBALA.BUN), /t/save the save overlay (with its own
 * bin.dat), /t/home $HOME.  Each answer is printed as a "T" line (the UG2
 * half's lines are compared with a build of the runtime before the change);
 * a failed check prints "FAIL"; the end is "regpath_unit: <n> passed, <m> failed". */
#include "cpu.h"
#include "game.h"
#include "kbd.h"
#include "module.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int pass, fail;
#define CHECK(cond, ...) do { if (cond) pass++; else { fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while (0)

/* ---- the import table: Windows' stdcall argument counts; both games import every one of these but
 * GetFileAttributesA, a path call of fileio.c's all the same ---- */
const ImpEnt g_imp[] = {
#define IAT(i) (0x00500000u + 4u * (i))
    { IAT(0),  "ADVAPI32.dll", "RegOpenKeyA", 3 },
    { IAT(1),  "ADVAPI32.dll", "RegOpenKeyExA", 5 },
    { IAT(2),  "ADVAPI32.dll", "RegCreateKeyA", 3 },
    { IAT(3),  "ADVAPI32.dll", "RegQueryValueExA", 6 },
    { IAT(4),  "ADVAPI32.dll", "RegSetValueExA", 6 },
    { IAT(5),  "ADVAPI32.dll", "RegCloseKey", 1 },
    { IAT(6),  "SHFOLDER.dll", "SHGetFolderPathA", 5 },
    { IAT(7),  "KERNEL32.dll", "GetModuleHandleA", 1 },
    { IAT(8),  "KERNEL32.dll", "GetModuleFileNameA", 3 },
    { IAT(9),  "KERNEL32.dll", "GetCurrentDirectoryA", 2 },
    { IAT(10), "KERNEL32.dll", "GetFullPathNameA", 4 },
    { IAT(11), "KERNEL32.dll", "GetCommandLineA", 0 },
    { IAT(12), "KERNEL32.dll", "CreateFileA", 7 },
    { IAT(13), "KERNEL32.dll", "ReadFile", 5 },
    { IAT(14), "KERNEL32.dll", "WriteFile", 5 },
    { IAT(15), "KERNEL32.dll", "CloseHandle", 1 },
    { IAT(16), "KERNEL32.dll", "GetFileAttributesA", 1 },
    { IAT(17), "KERNEL32.dll", "FindFirstFileA", 2 },
    { IAT(18), "KERNEL32.dll", "FindNextFileA", 2 },
    { IAT(19), "KERNEL32.dll", "FindClose", 1 },
    { IAT(20), "KERNEL32.dll", "CreateDirectoryA", 2 },
    { IAT(21), "KERNEL32.dll", "RemoveDirectoryA", 1 },
    { IAT(22), "KERNEL32.dll", "DeleteFileA", 1 },
    { IAT(23), "KERNEL32.dll", "MoveFileA", 2 },
    { IAT(24), "KERNEL32.dll", "GetLastError", 0 },
    { IAT(25), "KERNEL32.dll", "SetLastError", 1 },
    { IAT(26), "KERNEL32.dll", "GetLogicalDrives", 0 },
    { IAT(27), "KERNEL32.dll", "GetDriveTypeA", 1 },
    { IAT(28), "KERNEL32.dll", "GetLongPathNameA", 3 },
};
const uint32_t g_imp_count = sizeof g_imp / sizeof *g_imp;

/* ---- guest memory: [0, the program's data) is the guest's (the link's global base) ---- */
uint8_t g_cbits[1u << 15];
#define STACK_TOP  0x00800000u
#define TEB_MAIN   0x00700000u
#define STR        0x00600000u           /* the guest's strings, one 512-byte slot each, round robin */
#define BUF        0x00680000u           /* the answers' buffers */
#define RET_ADDR   0x00401234u
#define HKLM       0x80000002u
#define HKCU       0x80000001u
#define INVALID    0xFFFFFFFFu
#define GENERIC_READ  0x80000000u
#define GENERIC_WRITE 0x40000000u

/* ---- the game's table (its file is linked) and the rest of the runtime ---- */
/* the table names the lifted entry point and server.dll's tables: not linked here */
void GAMEINFO_ENTRY_FN(CPU *c){ (void)c; }
/* the race aids (career_mw.c, bot_mw.c) and the debugging hook (debug_mw.c) the table names: not linked here */
void mw_frame(CPU *c){ (void)c; }
void mw_bot_present(void){}                 /* the career bot (bot_mw.c, test only) */
int  mw_debug_hook(CPU *c, uint32_t va){ (void)c; (void)va; return 0; }
const ModInfo g_srv_modinfo; const ImpEnt g_srv_imp[1]; const uint32_t g_srv_imp_count;
const uint32_t g_srv_fn_addr[1]; const mod_fn g_srv_fn_ptr[1]; const uint32_t g_srv_fn_count;
#ifdef REGPATH_UG2
/* runtime/game_ug2.c also names UG2's features: none is linked or called here */
#define RT_STUBS_LANGUAGE "English"     /* runtime/language.c's answer for UG2's English */
void overrides_install(void){ } void language_install(void){ } void sponsors_install(void){ }
void lobbyclick_install(void){ } void menusize_install(void){ } void olfilter_install(void){ }
void tunegraph_install(void){ } void regional_install(void){ } void garage_install(void){ }
void carshadow_install(void){ } void hornmenu_install(void){ } void tonemenu_install(void){ } void powerunits_install(void){ }
int ws_hook(CPU *c, uint32_t a){ (void)c; (void)a; return 0; } int roam_hook(CPU *c, uint32_t a){ (void)c; (void)a; return 0; }
int language_hook(CPU *c, uint32_t a){ (void)c; (void)a; return 0; } int hornmenu_hook(CPU *c, uint32_t a){ (void)c; (void)a; return 0; }
int powerunits_hook(CPU *c, uint32_t a){ (void)c; (void)a; return 0; } int regional_hook(CPU *c, uint32_t a){ (void)c; (void)a; return 0; }
int garage_hook(CPU *c, uint32_t a){ (void)c; (void)a; return 0; } int tonemenu_hook(CPU *c, uint32_t a){ (void)c; (void)a; return 0; }
int sndlead_hook(CPU *c, uint32_t a){ (void)c; (void)a; return 0; } int drift_hook(CPU *c, uint32_t a){ (void)c; (void)a; return 0; }
void ug2_debug_present(CPU *c){ (void)c; } void ug2_debug_frame(void){ } void ug2_debug_sound_unlock(void){ }
void spinsleep_qpc(CPU *c){ (void)c; }
void itrace_dump(int n){ (void)n; } void watch_dump(const char *w, int n){ (void)w; (void)n; }
uint64_t g_gs_fail; uint32_t g_watch_addr;
void sms_request(int id){ (void)id; } int sms_delivered(void){ return 0; }
void photo_input(int cmd, int a, int b){ (void)cmd; (void)a; (void)b; }
void ug2_pad_rescan(int on){ (void)on; } unsigned ug2_pad_trigger_axes(uint32_t dev){ (void)dev; return 3u; }
float ug2_pad_jolt(void){ return 0.f; }
int career_fe_active(const char *name){ (void)name; return 0; } int hfr_rate_now(void){ return 0; }
int roam_exploring(void){ return 0; } int roam_game_cars(void){ return 0; } int roam_game_friends(void){ return 0; } int roam_game_engage(void){ return 0; } int roam_game_near(void){ return 0; }
uint64_t ws_now_us(void){ return 0; }
void sub_0074b730_orig(CPU *c){ (void)c; }
#endif
#define RT_STUBS_FILEIO_REAL
#include "rt_stubs.h"

int win32_call(CPU *c, uint32_t idx);
void win32_init(void);
void guest_lock(CPU *c);
void guest_unlock(void);
void fileio_set_root(const char *exe_path);
void fileio_set_write_root(const char *dir);

/* ---- calling an import as lifted code does ---- */
static CPU g_cpu;
static uint32_t idx_of(const char *name){
    for (uint32_t i = 0; i < g_imp_count; i++) if (!strcmp(g_imp[i].name, name)) return i;
    printf("FAIL: no import %s in the table\n", name); exit(2);
}
static uint32_t call_imp(const char *name, ...){
    uint32_t i = idx_of(name), n = (uint32_t)g_imp[i].nargs, a[16];
    va_list ap; va_start(ap, name);
    for (uint32_t k = 0; k < n; k++) a[k] = va_arg(ap, uint32_t);
    va_end(ap);
    CPU *c = &g_cpu;
    uint32_t base = STACK_TOP - 0x100u;
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
    CHECK(!g_unimpl[0], "%s: not implemented (\"%s\")", name, g_unimpl);
    return c->eax;
}
static uint32_t last_error(void){ return MR32(g_cpu.fs_base + 0x34u); }
static void set_last_error(uint32_t e){ MR32(g_cpu.fs_base + 0x34u) = e; }
static uint32_t g_strn;
static uint32_t gs(const char *s){                 /* the string in guest memory */
    uint32_t p = STR + 512u * (g_strn++ % 256u);
    snprintf((char *)(g_mem + p), 512, "%s", s);
    return p;
}
static const char *gstr(uint32_t p){ return (const char *)(g_mem + p); }
/* one "T" line: what was asked and what came back */
static void tr(const char *fmt, ...){
    char b[700]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("T %s\n", b);
}

/* ---- the registry ---- */
#define REG_SZ 1u
#define REG_DWORD 4u
#define MW_KEY "Software\\EA Games\\Need for Speed Most Wanted"
#define CPU_KEY "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0"
#define ERGC_KEY "Software\\Electronic Arts\\EA Games\\Need for Speed Most Wanted\\ergc"
static uint32_t open_ex(uint32_t root, const char *path, uint32_t access, uint32_t *err){
    uint32_t phk = BUF; MR32(phk) = 0xBAADF00Du;
    *err = call_imp("RegOpenKeyExA", root, path ? gs(path) : 0u, 0u, access, phk);
    tr("RegOpenKeyExA(0x%08x, \"%s\", 0x%x) -> %u, hkey %s", root, path ? path : "(null)", access, *err,
       MR32(phk) == 0 ? "0" : MR32(phk) == 0xBAADF00Du ? "untouched" : "set");
    return MR32(phk);
}
typedef struct { uint32_t r, type, cb; char s[300]; uint32_t dw; } Ans;
/* RegQueryValueExA with the game's buffer: cap bytes, type set beforehand as the game sets it (tin) */
static Ans query(uint32_t hk, const char *name, uint32_t tin, uint32_t cap){
    Ans a; memset(&a, 0, sizeof a);
    uint32_t pt = BUF + 0x10, pc = BUF + 0x14, pd = BUF + 0x100;
    MR32(pt) = tin; MR32(pc) = cap; memset(g_mem + pd, 0xEE, 300);
    a.r = call_imp("RegQueryValueExA", hk, name ? gs(name) : 0u, 0u, pt, cap ? pd : 0u, pc);
    a.type = MR32(pt); a.cb = MR32(pc); a.dw = MR32(pd);
    if (a.r == 0 && a.type == REG_SZ && cap) snprintf(a.s, sizeof a.s, "%s", gstr(pd));
    if (a.r == 0 && a.type == REG_DWORD) tr("query \"%s\" -> 0, REG_DWORD %u, cb %u", name ? name : "(default)", a.dw, a.cb);
    else if (a.r == 0) tr("query \"%s\" -> 0, type %u \"%s\", cb %u", name ? name : "(default)", a.type, a.s, a.cb);
    else tr("query \"%s\" -> %u, cb %u", name ? name : "(default)", a.r, a.cb);
    return a;
}
static uint32_t set_dword(uint32_t hk, const char *name, uint32_t v){
    MR32(BUF + 0x20) = v;
    uint32_t r = call_imp("RegSetValueExA", hk, name ? gs(name) : 0u, 0u, REG_DWORD, BUF + 0x20, 4u);
    tr("set \"%s\" REG_DWORD %u -> %u", name ? name : "(default)", v, r);
    return r;
}
static uint32_t set_raw(uint32_t hk, const char *name, uint32_t type, const void *data, uint32_t cb){
    memcpy(g_mem + BUF + 0x400, data, cb);
    uint32_t r = call_imp("RegSetValueExA", hk, name ? gs(name) : 0u, 0u, type, BUF + 0x400, cb);
    tr("set \"%s\" type %u, %u bytes -> %u", name ? name : "(default)", type, cb, r);
    return r;
}
static uint32_t close_key(uint32_t hk){
    uint32_t r = call_imp("RegCloseKey", hk);
    tr("RegCloseKey -> %u", r);
    return r;
}

/* the values LOAD_SETTINGS and SAVE_SETTINGS move, in their order; VTMode is loaded, never saved */
static const char *const SETTINGS[] = { "VERSION", "SIZE", "g_CarEnvironmentMapEnable", "g_CarEnvironmentMapUpdateData",
    "g_RoadReflectionEnable", "g_MotionBlurEnable", "g_ParticleSystemEnable", "g_WorldLodLevel", "g_CarLodLevel",
    "g_OverBrightEnable", "g_FSAALevel", "g_RainEnable", "g_TextureFiltering", "g_RacingResolution", "FirstTime",
    "g_PerformanceLevel", "g_VSyncOn", "g_ShadowDetail", "g_VisualTreatment", 0 };
/* what the auto-detect chose, for the test (VERSION and SIZE are the block's own; FirstTime is 1 on a first start) */
static const uint32_t CHOSEN[] = { 1, 132, 3, 1, 3, 1, 1, 3, 1, 1, 7, 1, 2, 1, 1, 5, 0, 2, 1 };

/* ---- the files ---- */
static uint32_t create(const char *path, uint32_t access, uint32_t disp){
    set_last_error(0);
    uint32_t h = call_imp("CreateFileA", gs(path), access, 1u, 0u, disp, 0x80u, 0u);
    tr("CreateFileA(\"%s\", 0x%08x, %u) -> %s, error %u", path, access, disp, h == INVALID ? "INVALID" : "a handle", last_error());
    return h;
}
static uint32_t attrs(const char *path){
    set_last_error(0);
    uint32_t r = call_imp("GetFileAttributesA", gs(path));
    tr("GetFileAttributesA(\"%s\") -> 0x%x, error %u", path, r, r == INVALID ? last_error() : 0u);
    return r;
}
static uint32_t find_first(const char *spec, char *name){
    set_last_error(0);
    uint32_t fd = BUF + 0x800, h = call_imp("FindFirstFileA", gs(spec), fd);
    if (name) snprintf(name, 260, "%s", h == INVALID ? "" : gstr(fd + 44));
    tr("FindFirstFileA(\"%s\") -> %s, error %u", spec, h == INVALID ? "INVALID" : gstr(fd + 44), h == INVALID ? last_error() : 0u);
    if (h != INVALID) call_imp("FindClose", h);
    return h;
}
/* a file that does not open, with Windows' error */
#define REFUSED(path, err) do { uint32_t h_ = create(path, GENERIC_READ, 3u); \
    CHECK(h_ == INVALID && last_error() == (err), "CreateFileA(\"%s\"): error %u, %u expected", path, last_error(), (uint32_t)(err)); \
    if (h_ != INVALID) call_imp("CloseHandle", h_); } while (0)
#define OPENS(path) do { uint32_t h_ = create(path, GENERIC_READ, 3u); \
    CHECK(h_ != INVALID, "CreateFileA(\"%s\") opens (error %u)", path, last_error()); \
    if (h_ != INVALID) call_imp("CloseHandle", h_); } while (0)
#ifndef REGPATH_UG2
/* ========== Most Wanted's registry ========== */
/* 0x7EC7A2: RegOpenKeyA(HKLM, CentralProcessor\0); ~MHz, type 4 and 4 bytes asked; close */
static uint32_t mw_mhz(void){
    uint32_t phk = BUF;
    uint32_t r = call_imp("RegOpenKeyA", HKLM, gs(CPU_KEY), phk), hk = MR32(phk);
    tr("RegOpenKeyA(HKLM, CentralProcessor\\0) -> %u", r);
    CHECK(r == 0 && hk, "the CPU's key opens (%u)", r);
    Ans a = query(hk, "~MHz", REG_DWORD, 4u);
    CHECK(a.r == 0 && a.type == REG_DWORD && a.cb == 4u, "~MHz is a REG_DWORD (%u, type %u, cb %u)", a.r, a.type, a.cb);
    CHECK(close_key(hk) == 0, "closed");
    return a.dw;
}
/* 0x6CB680: the start-up reads; FirstTime's answer (0 = there) */
static uint32_t mw_startup(const char *lang_expect){
    uint32_t e, hk = open_ex(HKLM, ERGC_KEY, 0x20019u, &e);
    CHECK(e == 2u && hk == 0, "the CD key's key is not there: error %u, hkey 0x%08x", e, hk);
    CHECK(close_key(hk) == 6u, "RegCloseKey of the 0 the failed open left: ERROR_INVALID_HANDLE");
    hk = open_ex(HKLM, MW_KEY, 0x2001Fu, &e);
    CHECK(e == 0 && hk, "the game's key opens with 0x2001F");
    Ans a = query(hk, "Install Dir", REG_SZ, 0x104u);
    CHECK(a.r == 0 && a.type == REG_SZ && !strcmp(a.s, "C:\\NFSMW") && a.cb == 9u, "Install Dir: \"%s\" (%u, cb %u)", a.s, a.r, a.cb);
    uint32_t lp = call_imp("GetLongPathNameA", BUF + 0x100, BUF + 0x100, 0x104u);      /* 0x6CB783 */
    CHECK(lp == 8u, "GetLongPathNameA(Install Dir): %u", lp);
    a = query(hk, "CD Drive", REG_SZ, 0x104u);
    CHECK(a.r == 2u && a.cb == 0, "CD Drive: not there (%u)", a.r);
    Ans ft = query(hk, "FirstTime", REG_SZ, 0x104u);
    static const uint8_t one[4] = { 1, 0, 0, 0 };                    /* 0x6CB847: REG_SZ, &dword 1, 4 bytes */
    CHECK(set_raw(hk, "FirstTime", REG_SZ, one, 4u) == 0, "FirstTime written as the game writes it");
    CHECK(query(hk, "CacheSize", REG_SZ, 0x104u).r == 2u, "CacheSize: not there");
    CHECK(query(hk, "SwapSize", REG_SZ, 0x104u).r == 2u, "SwapSize: not there");
    a = query(hk, "Language", REG_SZ, 0x104u);
    CHECK(a.r == 0 && a.type == REG_SZ && !strcmp(a.s, lang_expect), "Language: \"%s\", \"%s\" expected", a.s, lang_expect);
    CHECK(query(hk, "StreamingInstall", REG_SZ, 0x104u).r == 2u, "StreamingInstall: not there");
    CHECK(query(hk, "Locale", REG_SZ, 0x104u).r == 2u, "Locale: not there (speed.exe never asks)");
    CHECK(close_key(hk) == 0, "closed");
    return ft.r;
}
/* 0x6C1F10 SAVE_SETTINGS: KEY_ALL_ACCESS, 19 REG_DWORDs */
static void mw_save_settings(void){
    uint32_t e, hk = open_ex(HKLM, MW_KEY, 0xF003Fu, &e);
    CHECK(e == 0 && hk, "SAVE_SETTINGS opens the key (%u)", e);
    for (int i = 0; SETTINGS[i]; i++) CHECK(set_dword(hk, SETTINGS[i], CHOSEN[i]) == 0, "%s saved", SETTINGS[i]);
    CHECK(close_key(hk) == 0, "closed");
}
/* 0x6C1C00 LOAD_SETTINGS: KEY_READ, each value into the options block (4 bytes) */
static void mw_load_settings(void){
    uint32_t e, hk = open_ex(HKLM, MW_KEY, 0x20019u, &e);
    CHECK(e == 0 && hk, "LOAD_SETTINGS opens the key (%u)", e);
    for (int i = 0; SETTINGS[i]; i++){
        if (!strcmp(SETTINGS[i], "FirstTime")) continue;             /* not read here (0x6CB813 reads it) */
        Ans a = query(hk, SETTINGS[i], REG_DWORD, 4u);
        CHECK(a.r == 0 && a.type == REG_DWORD && a.cb == 4u && a.dw == CHOSEN[i], "%s: %u (REG_DWORD %u expected; r %u type %u)",
              SETTINGS[i], a.dw, CHOSEN[i], a.r, a.type);
    }
    Ans a = query(hk, "VTMode", REG_DWORD, 4u);
    CHECK(a.r == 2u && a.dw == 0xEEEEEEEEu, "VTMode: not there, the buffer untouched (the game keeps its 0)");
    CHECK(close_key(hk) == 0, "closed");
}
static void test_boot1(void){
    CHECK(mw_mhz() == 3000u, "~MHz: rdtsc's 3,000");
    CHECK(mw_startup("English US") == 2u, "a first start: FirstTime is not there");
    /* [0x901820] = 1: no LOAD_SETTINGS (0x6E6A66), the auto-detect runs, then SAVE_SETTINGS (0x6DFA1A) */
    mw_save_settings();
}
static void test_boot2(void){
    setenv("NFSU2_TSC_MHZ", "2400", 1);
    CHECK(mw_mhz() == 2400u, "~MHz follows NFSU2_TSC_MHZ, as rdtsc does");
    CHECK(mw_startup("German") == 0, "a later start: FirstTime is there");
    /* [0x901820] = 0: LOAD_SETTINGS runs, the auto-detect does not */
    mw_load_settings();
}
static void test_keys(void){
    uint32_t e, hk;
    /* no fakes: FirstTime is not there on a fresh registry, whatever the old switch says */
    hk = open_ex(HKLM, MW_KEY, 0x20019u, &e);
    CHECK(query(hk, "FirstTime", REG_SZ, 0x104u).r == 2u, "FirstTime not there with NFSU2_FAKE_NOTFIRSTTIME set");
    CHECK(query(hk, "NotFirstTime", REG_DWORD, 4u).r == 2u, "NotFirstTime (UG2's) not there either");
    Ans a = query(hk, "Language", REG_SZ, 0x104u);
    CHECK(!strcmp(a.s, "English US"), "a page language with no Most Wanted text: \"%s\"", a.s);
    /* sizes: too small a buffer, no buffer */
    a = query(hk, "Install Dir", REG_SZ, 4u);
    CHECK(a.r == 234u && a.cb == 9u, "REG_SZ into 4 bytes: ERROR_MORE_DATA, 9 needed (%u, %u)", a.r, a.cb);
    a = query(hk, "Install Dir", REG_SZ, 0);
    CHECK(a.r == 0 && a.cb == 9u && a.type == REG_SZ, "no buffer: the size (%u, %u)", a.r, a.cb);
    a = query(hk, "VERSION", REG_DWORD, 2u);
    CHECK(a.r == 234u && a.cb == 4u, "REG_DWORD into 2 bytes: ERROR_MORE_DATA (%u, %u)", a.r, a.cb);
    a = query(hk, "VERSION", REG_DWORD, 4u);
    CHECK(a.r == 0 && a.type == REG_DWORD && a.dw == 1u, "VERSION's default: REG_DWORD 1 (the options block's)");
    a = query(hk, "SIZE", REG_DWORD, 4u);
    CHECK(a.r == 0 && a.type == REG_DWORD && a.dw == 132u, "SIZE's default: REG_DWORD 132 (the options block's)");
    CHECK(query(hk, "g_Nothing", REG_DWORD, 4u).r == 2u, "a value nobody wrote: not there");
    CHECK(query(hk, 0, REG_SZ, 0x104u).r == 2u, "the key's default value: not there");
    /* L-056: no write through a handle opened for reading */
    CHECK(set_dword(hk, "g_VSyncOn", 1u) == 5u, "KEY_READ: RegSetValueExA refused");
    CHECK(query(hk, "g_VSyncOn", REG_DWORD, 4u).r == 2u, "and nothing was kept");
    close_key(hk);
    hk = open_ex(HKLM, MW_KEY, 0x2u, &e);                            /* KEY_SET_VALUE only */
    CHECK(e == 0 && set_dword(hk, "g_VSyncOn", 1u) == 0, "KEY_SET_VALUE: written");
    CHECK(query(hk, "g_VSyncOn", REG_DWORD, 4u).r == 5u, "no KEY_QUERY_VALUE: RegQueryValueExA refused");
    CHECK(set_dword(hk, 0, 1u) == 5u, "the default value is not kept");
    static const uint8_t bin[3] = { 1, 2, 3 };
    CHECK(set_raw(hk, "g_Bin", 3u, bin, 3u) == 50u, "REG_BINARY: not supported, said so");
    CHECK(set_raw(hk, "g_Short", REG_DWORD, bin, 2u) == 87u, "REG_DWORD of 2 bytes: ERROR_INVALID_PARAMETER");
    char longs[300]; memset(longs, 'x', sizeof longs - 1); longs[sizeof longs - 1] = 0;
    CHECK(set_raw(hk, "g_Long", REG_SZ, longs, sizeof longs) == 0, "a long REG_SZ is kept");
    CHECK(set_dword(hk, "VERSION", 7u) == 0 && set_raw(hk, "Install Dir", REG_SZ, "D:\\", 4u) == 0, "VERSION and Install Dir written");
    close_key(hk);
    hk = open_ex(HKLM, MW_KEY, 0x20019u, &e);
    a = query(hk, "g_Long", REG_SZ, 0x104u);
    CHECK(a.r == 0 && strlen(a.s) == 127u, "the long REG_SZ: its first 127 bytes (%zu)", strlen(a.s));
    CHECK(query(hk, "VERSION", REG_DWORD, 4u).dw == 7u, "a stored value before the default");
    CHECK(!strcmp(query(hk, "Install Dir", REG_SZ, 0x104u).s, "C:\\NFSMW"), "Install Dir stays the machine's");
    close_key(hk);
    /* keys: the same key spelled otherwise; keys the machine lacks */
    hk = open_ex(HKLM, "software\\\\EA GAMES\\need for speed most wanted\\", 0x20019u, &e);
    CHECK(e == 0 && hk, "another case and extra backslashes: the same key");
    uint32_t sub = open_ex(hk, "", 0x20019u, &e);
    CHECK(e == 0 && sub && sub != hk, "an empty subkey: the same key, a new handle");
    close_key(sub);
    open_ex(hk, "Sub", 0x20019u, &e);
    CHECK(e == 2u, "a subkey it lacks: error %u", e);
    close_key(hk);
    open_ex(HKCU, MW_KEY, 0x20019u, &e);
    CHECK(e == 2u, "the game's key under HKCU: not there (%u)", e);
    open_ex(HKLM, "Software\\EA Games", 0x20019u, &e);
    CHECK(e == 2u, "a parent key: not there (%u)", e);
    uint32_t phk = BUF; MR32(phk) = 0x1234u;
    CHECK(call_imp("RegCreateKeyA", HKLM, gs("Software\\Other"), phk) == 5u && MR32(phk) == 0, "RegCreateKeyA of a new key: refused");
    CHECK(call_imp("RegCreateKeyA", HKLM, gs(MW_KEY), phk) == 0 && MR32(phk), "RegCreateKeyA of the game's key opens it");
    close_key(MR32(phk));
    CHECK(call_imp("RegOpenKeyA", HKLM, 0u, phk) == 0 && MR32(phk) == HKLM, "RegOpenKeyA(HKLM, NULL): HKLM");
    CHECK(query(HKLM, "FirstTime", REG_SZ, 0x104u).r == 2u, "a value of HKLM itself: not there");
    /* the CPU's key keeps nothing */
    call_imp("RegOpenKeyA", HKLM, gs(CPU_KEY), phk);
    CHECK(set_dword(MR32(phk), "~MHz", 1u) == 5u, "~MHz cannot be written");
    close_key(MR32(phk));
    /* handles */
    CHECK(query(0x1234u, "VERSION", REG_DWORD, 4u).r == 6u, "a handle that is none: ERROR_INVALID_HANDLE");
    CHECK(set_dword(0x1234u, "VERSION", 1u) == 6u, "nor writes");
    CHECK(close_key(0x1234u) == 6u && close_key(HKLM) == 0, "RegCloseKey: a bad handle 6, a predefined key 0");
    hk = open_ex(HKLM, MW_KEY, 0x20019u, &e); close_key(hk);
    CHECK(query(hk, "VERSION", REG_DWORD, 4u).r == 6u, "a closed key's handle: ERROR_INVALID_HANDLE");
}
static void test_home(void){
    uint32_t e, hk = open_ex(HKLM, MW_KEY, 0x2001Fu, &e);
    CHECK(e == 0 && set_dword(hk, "g_RainEnable", 1u) == 0, "written without NFSU2_REGFILE");
    close_key(hk);
}

/* ========== Most Wanted's paths ========== */
static void test_paths(void){
    uint32_t b = BUF + 0x100, r;
    /* the exe, its folder, its names */
    r = call_imp("GetModuleFileNameA", 0u, b, 260u);
    CHECK(r == 18u && !strcmp(gstr(b), "C:\\NFSMW\\speed.exe"), "GetModuleFileNameA(NULL): \"%s\" (%u)", gstr(b), r);
    static const char *const exe[] = { "speed.exe", "SPEED.EXE", "speed", "C:\\NFSMW\\speed.exe", "kernel32.dll", 0 };
    for (int i = 0; exe[i]; i++){
        r = call_imp("GetModuleHandleA", gs(exe[i]));
        CHECK(r == 0x00400000u, "GetModuleHandleA(\"%s\"): 0x%08x", exe[i], r);
    }
    CHECK(call_imp("GetModuleHandleA", 0u) == 0x00400000u, "GetModuleHandleA(NULL): the exe");
    set_last_error(0);
    CHECK(call_imp("GetModuleHandleA", gs("speed2.exe")) == 0 && last_error() == 126u, "GetModuleHandleA(\"speed2.exe\"): not loaded, 126");
    r = call_imp("GetCurrentDirectoryA", 260u, b);
    CHECK(r == 8u && !strcmp(gstr(b), "C:\\NFSMW"), "GetCurrentDirectoryA: \"%s\" (%u)", gstr(b), r);
    r = call_imp("GetCommandLineA");
    CHECK(!strcmp(gstr(r), "speed.exe"), "GetCommandLineA: \"%s\"", gstr(r));
    r = call_imp("GetFullPathNameA", gs("bin.dat"), 260u, b, 0u);
    CHECK(r == 16u && !strcmp(gstr(b), "C:\\NFSMW\\bin.dat"), "GetFullPathNameA(\"bin.dat\"): \"%s\"", gstr(b));
    static const uint32_t csidl[] = { 0x8005u, 0x8027u, 0x001Au, 0x0005u };   /* the saves, the pictures, AppData */
    for (int i = 0; i < 4; i++){
        memset(g_mem + b, 0, 260);
        r = call_imp("SHGetFolderPathA", 0u, csidl[i], 0u, 0u, b);
        tr("SHGetFolderPathA(0x%x) -> %u \"%s\"", csidl[i], r, gstr(b));
        CHECK(r == 0 && !strcmp(gstr(b), "C:\\NFSMW"), "SHGetFolderPathA(0x%x): \"%s\" (%u)", csidl[i], gstr(b), r);
    }
    /* a save as sub_006CBF00 makes it: My Documents + "\NFS Most Wanted" + the profile, in the save overlay */
    CHECK(call_imp("CreateDirectoryA", gs("C:\\NFSMW\\NFS Most Wanted"), 0u) == 1, "the saves' folder");
    CHECK(call_imp("CreateDirectoryA", gs("C:\\NFSMW\\NFS Most Wanted\\ABC"), 0u) == 1, "a profile's folder");
    uint32_t h = create("C:\\NFSMW\\NFS Most Wanted\\ABC\\ABC", GENERIC_WRITE, 2u);
    CHECK(h != INVALID, "the profile's file");
    memcpy(g_mem + BUF + 0x40, "20CM", 4);
    CHECK(call_imp("WriteFile", h, BUF + 0x40, 4u, BUF + 0x48, 0u) == 1 && MR32(BUF + 0x48) == 4u, "written");
    call_imp("CloseHandle", h);
    char nm[260];
    CHECK(find_first("C:\\NFSMW\\NFS Most Wanted\\*", nm) != INVALID && !strcmp(nm, "ABC"), "the profile is listed: \"%s\"", nm);
    CHECK(attrs("C:\\NFSMW\\NFS Most Wanted\\ABC") == 0x10u && attrs("C:\\NFSMW\\NFS Most Wanted\\ABC\\ABC") == 0x80u, "folder and file");
    h = create("C:\\NFSMW\\nfsmw000.tga", GENERIC_WRITE, 2u);         /* My Pictures + "\nfsmw%03d.tga" (0x6C051A) */
    CHECK(h != INVALID, "a screenshot");
    call_imp("CloseHandle", h);
    /* the game folder's files, however the path is written */
    static const char *const in[] = { "GLOBAL\\GLOBALA.BUN", ".\\GLOBAL\\GLOBALA.BUN", "C:\\NFSMW\\GLOBAL\\GLOBALA.BUN",
        "c:/nfsmw/global/globala.bun", "\\NFSMW\\GLOBAL\\GLOBALA.BUN", "TRACKS\\..\\GLOBAL\\GLOBALA.BUN",
        "C:\\NFSMW\\..\\NFSMW\\GLOBAL\\GLOBALA.BUN", "C:GLOBAL\\GLOBALA.BUN", 0 };
    for (int i = 0; in[i]; i++) OPENS(in[i]);
    h = create("GLOBAL\\GLOBALA.BUN", GENERIC_READ, 3u);
    MR8(BUF + 0x40) = 0;
    CHECK(call_imp("ReadFile", h, BUF + 0x40, 1u, BUF + 0x48, 0u) == 1 && MR8(BUF + 0x40) == 'g', "read from the game folder");
    call_imp("CloseHandle", h);
    OPENS("bin.dat"); OPENS("C:\\NFSMW\\bin.dat");                  /* there, in the folder: the probe is about the drive */
    /* the disc probe: sub_006C25A0 builds "<drive>:\" + "bin.dat" and tests it (0x6C25FA), and the drive scan
     * 0x6C2620 runs it on the drives GetLogicalDrives reports (v1.3's [0x8A9260] = 1: every one not remote) */
    uint32_t drives = call_imp("GetLogicalDrives");
    CHECK(drives == 0x0Cu, "GetLogicalDrives: C: and D: (0x%x)", drives);
    for (int l = 0; l < 26; l++) if (drives & (1u << l)){
        char root[8]; snprintf(root, sizeof root, "%c:\\", 'A' + l);
        CHECK(call_imp("GetDriveTypeA", gs(root)) == 3u, "GetDriveTypeA(\"%s\"): DRIVE_FIXED", root);
    }
    for (int l = 0; l < 26; l++){
        for (int lower = 0; lower < 2; lower++){
            char p[16]; snprintf(p, sizeof p, "%c:\\bin.dat", (lower ? 'a' : 'A') + l);
            uint32_t want = l == 2 || l == 3 ? 2u : 3u;             /* the root of C: or D:: no such file; any other drive: none */
            REFUSED(p, want);
            CHECK(attrs(p) == INVALID && last_error() == want, "GetFileAttributesA(\"%s\"): error %u", p, last_error());
            CHECK(find_first(p, 0) == INVALID && last_error() == want, "FindFirstFileA(\"%s\"): error %u", p, last_error());
        }
    }
    /* nothing else outside C:\NFSMW either */
    REFUSED("..\\bin.dat", 2u); REFUSED("C:\\NFSMW\\..\\bin.dat", 2u); REFUSED("GLOBAL\\..\\..\\bin.dat", 2u);
    REFUSED("..\\..\\..\\..\\etc\\passwd", 3u); REFUSED("\\bin.dat", 2u); REFUSED("/bin.dat", 2u); REFUSED("/etc/passwd", 3u);
    REFUSED("C:\\NFSMW2\\bin.dat", 3u); REFUSED("C:\\Windows\\win.ini", 3u); REFUSED("D:bin.dat", 2u); REFUSED("Q:bin.dat", 3u);
    REFUSED("\\\\server\\share\\bin.dat", 3u); REFUSED("//server/share/bin.dat", 3u); REFUSED("\\\\.\\PhysicalDrive0", 3u);
    REFUSED("\\\\?\\C:\\NFSMW\\bin.dat", 3u); REFUSED("NUL", 2u); REFUSED("C:\\NFSMW\\con.txt", 2u); REFUSED("COM1", 2u);
    REFUSED("lpt9.log", 2u); REFUSED("GLOBAL\\aux", 2u); REFUSED("bin.dat:stream", 123u); REFUSED("bin*.dat", 123u);
    /* and no write reaches outside */
    CHECK(create("D:\\evil.txt", GENERIC_WRITE, 2u) == INVALID && last_error() == 2u, "CreateFileA(\"D:\\evil.txt\", CREATE_ALWAYS): refused");
    CHECK(create("..\\evil.txt", GENERIC_WRITE, 2u) == INVALID, "CreateFileA(\"..\\evil.txt\", CREATE_ALWAYS): refused");
    CHECK(call_imp("CreateDirectoryA", gs("..\\evil"), 0u) == 0 && last_error() == 2u, "CreateDirectoryA(\"..\\evil\"): refused");
    CHECK(call_imp("CreateDirectoryA", gs("E:\\evil"), 0u) == 0 && last_error() == 3u, "CreateDirectoryA(\"E:\\evil\"): refused");
    CHECK(call_imp("DeleteFileA", gs("C:\\bin.dat")) == 0 && last_error() == 2u, "DeleteFileA(\"C:\\bin.dat\"): refused");
    CHECK(call_imp("DeleteFileA", gs("..\\NFSMW\\..\\bin.dat")) == 0, "DeleteFileA through '..': refused");
    CHECK(call_imp("MoveFileA", gs("bin.dat"), gs("D:\\bin.dat")) == 0 && last_error() == 2u, "MoveFileA to D:: refused");
    CHECK(call_imp("MoveFileA", gs("D:\\bin.dat"), gs("moved.dat")) == 0 && last_error() == 2u, "MoveFileA from D:: refused");
    CHECK(call_imp("RemoveDirectoryA", gs("D:\\")) == 0, "RemoveDirectoryA(\"D:\\\"): refused");
    CHECK(find_first("D:\\*", 0) == INVALID && last_error() == 2u, "FindFirstFileA(\"D:\\*\"): nothing");
    CHECK(find_first("..\\*", 0) == INVALID && last_error() == 2u, "FindFirstFileA(\"..\\*\"): nothing");
    CHECK(find_first("\\\\server\\share\\*", 0) == INVALID && last_error() == 3u, "FindFirstFileA of a share: nothing");
    CHECK(find_first("*", nm) != INVALID, "FindFirstFileA(\"*\"): the current directory's files");
    CHECK(find_first("C:\\NFSMW\\GLOBAL\\*.BUN", nm) != INVALID && !strcasecmp(nm, "GLOBALA.BUN"), "FindFirstFileA in the folder: \"%s\"", nm);
}
/* no save overlay: the game folder is read-only for a sandboxed game */
static void test_ro(void){
    CHECK(create("C:\\NFSMW\\new.txt", GENERIC_WRITE, 2u) == INVALID && last_error() == 5u, "CreateFileA(CREATE_ALWAYS) in the game folder: refused, 5");
    CHECK(create("bin.dat", GENERIC_READ | GENERIC_WRITE, 3u) == INVALID && last_error() == 5u, "bin.dat opened for writing: refused, 5");
    CHECK(call_imp("CreateDirectoryA", gs("C:\\NFSMW\\NFS Most Wanted"), 0u) == 0, "CreateDirectoryA in the game folder: refused");
    CHECK(call_imp("DeleteFileA", gs("bin.dat")) == 0 && last_error() == 5u, "DeleteFileA in the game folder: refused, 5");
    CHECK(call_imp("MoveFileA", gs("bin.dat"), gs("moved.dat")) == 0, "MoveFileA in the game folder: refused");
    OPENS("bin.dat");
}
/* a whole folder scan: FindFirstFileA, FindNextFileA to the end, FindClose; the names, or -1 for no handle */
static int scan(const char *spec, char got[][64], int max){
    uint32_t fd = BUF + 0x800; int n = 0;
    set_last_error(0);
    uint32_t h = call_imp("FindFirstFileA", gs(spec), fd);
    if (h == INVALID) return -1;
    do { if (n < max) snprintf(got[n], 64, "%s", gstr(fd + 44)); n++; } while (call_imp("FindNextFileA", h, fd));
    CHECK(last_error() == 18u, "%s: the scan ends with ERROR_NO_MORE_FILES, not %u", spec, last_error());
    CHECK(call_imp("FindClose", h) == 1, "%s: FindClose", spec);
    char line[600] = ""; for (int i = 0; i < n && i < max; i++){ strcat(line, " "); strncat(line, got[i], 40); }
    tr("scan \"%s\":%s", spec, line);
    return n;
}
/* NTFS's order for ASCII names: upper-cased, then by byte */
static int ntfs_before(const char *a, const char *b){
    for (;; a++, b++){
        int x = toupper((unsigned char)*a), y = toupper((unsigned char)*b);
        if (x != y || !x) return x < y;
    }
}
/* folder scans in NTFS order (game.h find_sorted): MW lists its aliases in the order the scan gives them (GAME.md 10) */
static void test_scan(void){
    static const char *const made[] = { "bravo", "CHARLIE", "_x", "Alpha", "AB", "a_b" };
    static const char *const want[] = { "AB", "Alpha", "a_b", "bravo", "CHARLIE", "_x" };
    char got[16][64], p[300];
    CHECK(call_imp("CreateDirectoryA", gs("C:\\NFSMW\\NFS Most Wanted"), 0u) == 1, "CreateDirectoryA of the save folder");
    for (int i = 0; i < 6; i++){
        snprintf(p, sizeof p, "C:\\NFSMW\\NFS Most Wanted/%s", made[i]);
        CHECK(call_imp("CreateDirectoryA", gs(p), 0u) == 1, "CreateDirectoryA(\"%s\")", p);
    }
    int n = scan("C:\\NFSMW\\NFS Most Wanted\\*", got, 16);
    CHECK(n == 6, "the alias folder scan: %d names, 6 expected", n);
    for (int i = 0; i < 6 && i < n; i++) CHECK(!strcmp(got[i], want[i]), "name %d: \"%s\", \"%s\" expected", i, got[i], want[i]);
    /* a deleted one is gone from the next scan */
    CHECK(call_imp("RemoveDirectoryA", gs("C:\\NFSMW\\NFS Most Wanted/Alpha")) == 1, "RemoveDirectoryA of Alpha");
    n = scan("C:\\NFSMW\\NFS Most Wanted/*", got, 16);
    CHECK(n == 5, "after the delete: %d names, 5 expected", n);
    for (int i = 0; i < n && i < 16; i++) CHECK(strcmp(got[i], "Alpha"), "the deleted Alpha is in the scan");
    /* the game folder and the save overlay in one scan, in one order; bin.dat, in both, once */
    n = scan("C:\\NFSMW\\*", got, 16);
    int dat = 0, nfs = 0, glob = 0;
    for (int i = 0; i < n && i < 16; i++){
        dat += !strcasecmp(got[i], "bin.dat"); nfs += !strcmp(got[i], "NFS Most Wanted"); glob += !strcmp(got[i], "GLOBAL");
        if (i) CHECK(ntfs_before(got[i - 1], got[i]), "\"%s\" before \"%s\": not NTFS order", got[i - 1], got[i]);
    }
    CHECK(dat == 1 && nfs == 1 && glob == 1, "the root scan: bin.dat %d, NFS Most Wanted %d, GLOBAL %d times", dat, nfs, glob);
    /* a pattern, and a scan that matches nothing */
    n = scan("C:\\NFSMW\\NFS Most Wanted\\?_*", got, 16);
    CHECK(n == 1 && !strcmp(got[0], "a_b"), "the pattern ?_*: %d names", n);
    CHECK(scan("C:\\NFSMW\\NFS Most Wanted\\zz*", got, 16) == -1 && last_error() == 2u, "no match: no handle, error %u", last_error());
    /* a scan left before its end: FindClose frees it, and so does CloseHandle */
    uint32_t fd = BUF + 0x800, h = call_imp("FindFirstFileA", gs("C:\\NFSMW\\NFS Most Wanted\\*"), fd);
    CHECK(h != INVALID && !strcmp(gstr(fd + 44), "AB"), "the first name again: \"%s\"", gstr(fd + 44));
    CHECK(call_imp("FindClose", h) == 1, "FindClose before the end");
    h = call_imp("FindFirstFileA", gs("C:\\NFSMW\\NFS Most Wanted\\*"), fd);
    CHECK(h != INVALID && call_imp("CloseHandle", h) == 1, "CloseHandle of a scan");
}
#else
/* ========== UG2: the flat registry and its paths, as they always were ========== */
#define UG2_KEY "Software\\EA Games\\Need for Speed Underground 2"
static void test_ug2(void){
    uint32_t e, hk, phk = BUF; Ans a;
    /* the flat registry: every key opens, a value is the stored one, then the game's answers */
    CHECK(call_imp("RegOpenKeyA", HKLM, gs(CPU_KEY), phk) == 0 && MR32(phk), "RegOpenKeyA");
    a = query(MR32(phk), "~MHz", REG_DWORD, 4u);
    CHECK(a.r == 0 && a.type == REG_DWORD && a.dw == 3000u, "~MHz 3000");
    close_key(MR32(phk));
    hk = open_ex(HKLM, ERGC_KEY, 0x20019u, &e);
    CHECK(e == 0 && hk, "any key opens");
    CHECK(query(hk, 0, REG_SZ, 0x15u).r == 2u, "the default value: not there");
    close_key(hk);
    hk = open_ex(HKLM, UG2_KEY, 0x2001Fu, &e);
    a = query(hk, "Install Dir", REG_SZ, 0x104u);
    CHECK(a.r == 0 && a.type == REG_SZ && !strcmp(a.s, "C:\\NFSU2"), "Install Dir \"%s\"", a.s);
    a = query(hk, "CD Drive", REG_SZ, 0x104u);
    CHECK(a.r == 0 && !strcmp(a.s, "C:\\NFSU2"), "CD Drive \"%s\"", a.s);
    a = query(hk, "VERSION", REG_SZ, 0x104u);
    CHECK(a.r == 0 && a.type == REG_SZ && !strcmp(a.s, "1.2.0.0") && a.cb == 8u, "VERSION \"%s\" REG_SZ", a.s);
    a = query(hk, "VERSION", REG_DWORD, 4u);
    CHECK(a.r == 234u && a.cb == 8u, "VERSION into 4 bytes: ERROR_MORE_DATA");
    a = query(hk, "SIZE", REG_DWORD, 4u);
    CHECK(a.r == 0 && a.type == REG_DWORD && a.dw == 0, "SIZE REG_DWORD 0");
    a = query(hk, "StreamingInstall", REG_DWORD, 4u);
    CHECK(a.r == 0 && a.type == REG_DWORD && a.dw == 0, "StreamingInstall REG_DWORD 0");
    a = query(hk, "Language", REG_SZ, 0x104u);
    CHECK(a.r == 0 && !strcmp(a.s, "English"), "Language \"%s\" (language.c's)", a.s);
    CHECK(query(hk, "NotFirstTime", REG_DWORD, 4u).r == 2u, "NotFirstTime not there");
    CHECK(query(hk, "FirstTime", REG_SZ, 0x104u).r == 2u, "FirstTime not there");
    CHECK(set_dword(hk, "NotFirstTime", 1u) == 0 && query(hk, "NotFirstTime", REG_DWORD, 4u).dw == 1u, "NotFirstTime written and read");
    CHECK(set_raw(hk, "Language", REG_SZ, "French", 7u) == 0 && !strcmp(query(hk, "Language", REG_SZ, 0x104u).s, "English"),
          "Language stays the setting's");
    CHECK(set_raw(hk, "g_Text", REG_SZ, "Hello", 6u) == 0 && !strcmp(query(hk, "g_Text", REG_SZ, 0x104u).s, "Hello"), "a REG_SZ kept");
    static const uint8_t bin[3] = { 1, 2, 3 };
    CHECK(set_raw(hk, "g_Bin", 3u, bin, 3u) == 0 && query(hk, "g_Bin", 3u, 0x104u).r == 2u, "REG_BINARY: 0 back, nothing kept");
    CHECK(query(hk, "g_Nothing", REG_DWORD, 4u).r == 2u, "a value nobody wrote");
    for (int i = 0; SETTINGS[i]; i++) set_dword(hk, SETTINGS[i], CHOSEN[i]);
    for (int i = 0; SETTINGS[i]; i++) CHECK(query(hk, SETTINGS[i], REG_DWORD, 4u).dw == CHOSEN[i], "%s round trip", SETTINGS[i]);
    close_key(hk);
    hk = open_ex(HKLM, UG2_KEY, 0x20019u, &e);
    CHECK(set_dword(hk, "g_VSyncOn", 9u) == 0 && query(hk, "g_VSyncOn", REG_DWORD, 4u).dw == 9u, "a KEY_READ handle writes too");
    close_key(hk);
    open_ex(HKCU, "No\\Such\\Key", 0x20019u, &e);
    CHECK(e == 0, "HKCU\\No\\Such\\Key opens");
    CHECK(call_imp("RegCreateKeyA", HKLM, gs("Software\\Other"), phk) == 0 && MR32(phk), "RegCreateKeyA of any key");
    CHECK(close_key(0x1234u) == 0, "RegCloseKey of anything: 0");
    CHECK(query(0x1234u, "SIZE", REG_DWORD, 4u).r == 0, "any handle answers");
    /* the paths: the exe, the folder, and every path maps into the game folder */
    uint32_t b = BUF + 0x100, r;
    r = call_imp("GetModuleFileNameA", 0u, b, 260u);
    CHECK(!strcmp(gstr(b), "C:\\\\NFSU2\\\\SPEED2.EXE"), "GetModuleFileNameA: \"%s\" (doubled backslashes, as always)", gstr(b));
    tr("GetModuleFileNameA -> %u \"%s\"", r, gstr(b));
    static const char *const exe[] = { "SPEED2.EXE", "speed2.exe", "speed2", "speed.exe", 0 };
    for (int i = 0; exe[i]; i++){ set_last_error(0); r = call_imp("GetModuleHandleA", gs(exe[i]));
        tr("GetModuleHandleA(\"%s\") -> 0x%08x, error %u", exe[i], r, last_error()); }
    r = call_imp("GetCurrentDirectoryA", 260u, b);
    CHECK(r == 8u && !strcmp(gstr(b), "C:\\NFSU2"), "GetCurrentDirectoryA \"%s\"", gstr(b));
    r = call_imp("GetCommandLineA");
    CHECK(!strcmp(gstr(r), "SPEED2.EXE"), "GetCommandLineA \"%s\"", gstr(r));
    r = call_imp("SHGetFolderPathA", 0u, 0x8005u, 0u, 0u, b);
    CHECK(r == 0 && !strcmp(gstr(b), "C:\\NFSU2"), "SHGetFolderPathA \"%s\"", gstr(b));
    r = call_imp("GetFullPathNameA", gs("bin.dat"), 260u, b, 0u);
    tr("GetFullPathNameA(\"bin.dat\") -> %u \"%s\"", r, gstr(b));
    CHECK(call_imp("CreateDirectoryA", gs("C:\\NFSU2\\NFS Underground 2"), 0u) == 1 &&
          call_imp("CreateDirectoryA", gs("C:\\NFSU2\\NFS Underground 2\\ABC"), 0u) == 1, "the saves' folders");
    uint32_t h = create("C:\\NFSU2\\NFS Underground 2\\ABC\\ABC", GENERIC_WRITE, 2u);
    CHECK(h != INVALID, "a profile's file");
    call_imp("CloseHandle", h);
    static const char *const any[] = { "bin.dat", "C:\\NFSU2\\bin.dat", "C:\\bin.dat", "D:\\bin.dat", "Q:\\bin.dat",
        "..\\bin.dat", "..\\..\\..\\bin.dat", "\\bin.dat", "C:\\NFSU2\\..\\bin.dat", "GLOBAL\\GLOBALA.BUN",
        "NFSU2\\GLOBAL\\GLOBALA.BUN", 0 };
    for (int i = 0; any[i]; i++) OPENS(any[i]);
    static const char *const none[] = { "\\\\server\\share\\bin.dat", "NUL", "bin.dat:stream", "C:\\Windows\\win.ini", 0 };
    for (int i = 0; none[i]; i++){ uint32_t x = create(none[i], GENERIC_READ, 3u); if (x != INVALID) call_imp("CloseHandle", x); }
    for (int i = 0; any[i]; i++) attrs(any[i]);
    find_first("D:\\*", 0); find_first("..\\*", 0); find_first("*", 0);
    CHECK(call_imp("GetLogicalDrives") == 0x0Cu, "GetLogicalDrives");
    h = create("D:\\fromd.txt", GENERIC_WRITE, 2u);                   /* D:\ is the game folder too */
    CHECK(h != INVALID, "CreateFileA(\"D:\\fromd.txt\", CREATE_ALWAYS): in the save overlay");
    call_imp("CloseHandle", h);
}
static void test_ug2home(void){
    uint32_t e, hk = open_ex(HKLM, UG2_KEY, 0x2001Fu, &e);
    CHECK(set_dword(hk, "g_RainEnable", 1u) == 0, "written without NFSU2_REGFILE");
    close_key(hk);
}
static void test_ug2nosave(void){
    uint32_t h = create("C:\\NFSU2\\new.txt", GENERIC_WRITE, 2u);
    CHECK(h != INVALID, "CreateFileA(CREATE_ALWAYS) in the game folder: allowed without a save overlay, as always");
    if (h != INVALID) call_imp("CloseHandle", h);
}
#endif

int main(int argc, char **argv){
    const char *mode = argc > 1 ? argv[1] : "";
    memset(g_cbits, 0xFF, 64u << 20 >> 14 >> 3);                      /* the first 64 MB are committed */
    /* the environment the page gives (wasi-run.mjs passes none) */
    int home = !strcmp(mode, "home") || !strcmp(mode, "ug2home");
    setenv("HOME", "/t/home", 1);
    if (!home) setenv("NFSU2_REGFILE", !strcmp(mode, "keys") ? "/t/save/keys.txt" : !strcmp(mode, "ug2") ? "/t/save/ug2.txt"
                                       : "/t/save/registry.txt", 1);
    if (!strcmp(mode, "boot2")) setenv("NFSU2_LANGUAGE", "german", 1);
    if (!strcmp(mode, "keys")){ setenv("NFSU2_LANGUAGE", "russian", 1); setenv("NFSU2_FAKE_NOTFIRSTTIME", "1", 1); }
#ifndef REGPATH_UG2
    fileio_set_root("/t/game/speed.exe");
    if (strcmp(mode, "ro")) fileio_set_write_root("/t/save");
#else
    fileio_set_root("/t/game/SPEED2.EXE");
    if (strcmp(mode, "ug2nosave")) fileio_set_write_root("/t/save");
#endif
    win32_init();
    memset(&g_cpu, 0, sizeof g_cpu);
    g_cpu.fs_base = TEB_MAIN; g_cpu.fcw = 0x027f;
    memset(g_mem + TEB_MAIN, 0, 0x1000);
    MR32(TEB_MAIN + 0x18u) = TEB_MAIN; MR32(TEB_MAIN + 0x24u) = 100u;
    guest_lock(&g_cpu);
#ifndef REGPATH_UG2
    if (!strcmp(mode, "boot1")) test_boot1();
    else if (!strcmp(mode, "boot2")) test_boot2();
    else if (!strcmp(mode, "keys")) test_keys();
    else if (!strcmp(mode, "home")) test_home();
    else if (!strcmp(mode, "paths")) test_paths();
    else if (!strcmp(mode, "ro")) test_ro();
    else if (!strcmp(mode, "scan")) test_scan();
    else { printf("usage: regpath_mw.wasm boot1|boot2|keys|home|paths|ro|scan\n"); return 2; }
#else
    if (!strcmp(mode, "ug2")) test_ug2();
    else if (!strcmp(mode, "ug2home")) test_ug2home();
    else if (!strcmp(mode, "ug2nosave")) test_ug2nosave();
    else { printf("usage: regpath_ug2.wasm ug2|ug2home|ug2nosave\n"); return 2; }
#endif
    guest_unlock();
    printf("regpath_unit: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
