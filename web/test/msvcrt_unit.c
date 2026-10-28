/* Most Wanted's msvcrt.dll (TRACKER M2-07): runtime/mw/msvcrt_mw.c, the C
 * runtime of the D3DX oracle, called as the oracle's lifted code calls it.
 * runtime/win32.c, win32_ext.c, win32_misc.c, keyboard.c, module.c and
 * modheap.c are the real ones, and so is Most Wanted's table (game_mw.c with
 * the oracle's entry: -DNFSU2_D3DX_ORACLE=1); the rest of the runtime is
 * web/test/rt_stubs.h.  The oracle's lift is not linked: its module entry
 * has the 63 msvcrt imports of d3dx9_26.dll's import table (cdecl: 0 popped,
 * which web/test/msvcrt-wasm.test.mjs checks against argcount's KNOWN and
 * the lift) and no functions.  Each import goes through win32_call with its
 * index in the module's part of the import table, so it is served as the
 * DLL's (its heap is the DLL's own part of the modheap region); after each
 * call the stack pointer is where a cdecl caller pops from and the caller's
 * frame is untouched.  Guest functions (qsort's comparator, _initterm's
 * table) are host functions here, at guest addresses (dispatch_indirect).
 *
 *   msvcrt_unit.wasm crt          every import against the C standard's and Microsoft's documented results
 *   msvcrt_unit.wasm stop <name>  one of the imports that end the process (terminate, _purecall,
 *                                 _except_handler3, __CxxFrameHandler, longjmp): its line and exit code
 *
 * Results are "T" lines; a failed check prints "FAIL"; the end is
 * "msvcrt_unit: <n> passed, <m> failed". */
#include "cpu.h"
#include "game.h"
#include "kbd.h"
#include "memmap.h"
#include "module.h"
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pass, fail;
#define CHECK(cond, ...) do { if (cond) pass++; else { fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while (0)

/* ---- the exe's imports: one; the oracle's: the 63 of msvcrt.dll, as its import table lists them ---- */
const ImpEnt g_imp[] = { { 0x00500000u, "KERNEL32.dll", "GetLastError", 0 } };
const uint32_t g_imp_count = 1;
#define CRT(i, n) { MM_D3DX_LO + 0x11a4u + 4u * (i), "msvcrt.dll", n, 0 }
const ImpEnt g_d3dx_imp[] = {
    CRT(0, "_except_handler3"), CRT(1, "?terminate@@YAXXZ"), CRT(2, "_adjust_fdiv"), CRT(3, "_initterm"), CRT(4, "srand"),
    CRT(5, "_tempnam"), CRT(6, "exit"), CRT(7, "atol"), CRT(8, "_ultoa"), CRT(9, "wcstombs"), CRT(10, "rand"),
    CRT(11, "isxdigit"), CRT(12, "atof"), CRT(13, "_CIcosh"), CRT(14, "_CIexp"), CRT(15, "_CIsinh"), CRT(16, "_CItanh"),
    CRT(17, "_fpclass"), CRT(18, "strchr"), CRT(19, "_isnan"), CRT(20, "isalnum"), CRT(21, "isalpha"), CRT(22, "toupper"),
    CRT(23, "tolower"), CRT(24, "atoi"), CRT(25, "floor"), CRT(26, "_strtime"), CRT(27, "_strdate"), CRT(28, "sscanf"),
    CRT(29, "isspace"), CRT(30, "isdigit"), CRT(31, "_setjmp3"), CRT(32, "longjmp"), CRT(33, "ldexp"), CRT(34, "frexp"),
    CRT(35, "wcslen"), CRT(36, "calloc"), CRT(37, "realloc"), CRT(38, "malloc"), CRT(39, "_CIsqrt"), CRT(40, "setlocale"),
    CRT(41, "_strdup"), CRT(42, "free"), CRT(43, "ceil"), CRT(44, "_controlfp"), CRT(45, "_vsnprintf"), CRT(46, "_CIasin"),
    CRT(47, "__CxxFrameHandler"), CRT(48, "_finite"), CRT(49, "_CIacos"), CRT(50, "memmove"), CRT(51, "qsort"),
    CRT(52, "_CIfmod"), CRT(53, "_purecall"), CRT(54, "_stricmp"), CRT(55, "modf"), CRT(56, "iswspace"),
    CRT(57, "iswalpha"), CRT(58, "iswdigit"), CRT(59, "iswpunct"), CRT(60, "_CIpow"), CRT(61, "??2@YAPAXI@Z"),
    CRT(62, "??3@YAXPAX@Z"),
};
const uint32_t g_d3dx_imp_count = sizeof g_d3dx_imp / sizeof *g_d3dx_imp;
const ModInfo g_d3dx_modinfo = { "d3dx9_26.dll", "-", MM_D3DX_LO, 0x00400000u, 0x0024a000u, 0, 0, 0, 0 };
const uint32_t g_d3dx_fn_addr[1]; const mod_fn g_d3dx_fn_ptr[1]; const uint32_t g_d3dx_fn_count;

/* ---- guest memory: below the program's data (the link's global base, 0x28000000) ---- */
uint8_t g_cbits[1u << 15];              /* every page counts as committed: the memory is all there */
#define STACK_TOP  0x00800000u
#define TEB_MAIN   0x00700000u
#define STR        0x00600000u           /* the guest's strings */
#define BUF        0x06000000u           /* answers' buffers and arrays (1 MB) */
#define RET_ADDR   0x00D81234u           /* inside the oracle's slot: its code calls */
#define FN_CMP     0x00D90000u           /* "guest functions": qsort's comparator, _initterm's table */
#define FN_INIT1   0x00D90010u
#define FN_INIT2   0x00D90020u
#define FN_CMPS    0x00D90100u           /* more comparators: guest_cmp at FN_CMPS + 16 k, k < N_CMPS */
#define N_CMPS     11u

/* ---- the game's table (its file is linked) and the rest of the runtime ---- */
void GAMEINFO_ENTRY_FN(CPU *c){ (void)c; }
const ModInfo g_srv_modinfo; const ImpEnt g_srv_imp[1]; const uint32_t g_srv_imp_count;
const uint32_t g_srv_fn_addr[1]; const mod_fn g_srv_fn_ptr[1]; const uint32_t g_srv_fn_count;
#define RT_STUBS_MODULE_REAL
#define RT_STUBS_DISPATCH_OWN
#include "rt_stubs.h"

int win32_call(CPU *c, uint32_t idx);
void win32_init(void);
extern jmp_buf g_exit_jmp; extern int g_exit_code, g_exit_armed;

/* "guest functions" (cdecl): what dispatch_indirect runs for their addresses */
static uint32_t g_arr, g_arr_end, g_cmp_calls, g_cmp_outside, g_init_order;
static void guest_cmp(CPU *c){                       /* int cmp(const int *a, const int *b) */
    uint32_t a = MR32(c->esp + 4), b = MR32(c->esp + 8);
    g_cmp_calls++;
    if (a < g_arr || a >= g_arr_end || b < g_arr || b >= g_arr_end) g_cmp_outside++;
    int32_t x = (int32_t)MR32(a), y = (int32_t)MR32(b);
    c->eax = (uint32_t)(x < y ? -1 : x > y);
    c->ecx = 0xBADBAD;                               /* a callee's scratch registers: the caller's come back */
    POP32(c->eip);
}
static void guest_init1(CPU *c){ g_init_order = g_init_order * 10u + 1u; c->fcw = 0x007F; POP32(c->eip); }
static void guest_init2(CPU *c){ g_init_order = g_init_order * 10u + 2u; POP32(c->eip); }
void dispatch_indirect(CPU *c, uint32_t a){
    if (a == FN_CMP) guest_cmp(c);
    else if (a - FN_CMPS < 16u * N_CMPS && !(a & 15u)) guest_cmp(c);
    else if (a == FN_INIT1) guest_init1(c);
    else if (a == FN_INIT2) guest_init2(c);
    else { printf("FAIL dispatch_indirect: no guest function at 0x%08x\n", a); fail++; POP32(c->eip); }
}
int g_guest_exited;                      /* dispatch.c's */
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args){
    CPU save = *c;
    for (int i = nargs - 1; i >= 0; i--) PUSH32(args[i]);
    PUSH32(0xDEADBEEFu);
    dispatch_indirect(c, fn);
    uint32_t r = c->eax; *c = save; return r;
}

/* ---- calling an import as the DLL's lifted code does ---- */
static CPU g_cpu;
static uint32_t idx_of(const char *name){
    for (uint32_t i = 0; i < g_d3dx_imp_count; i++) if (!strcmp(g_d3dx_imp[i].name, name)) return module_imp_first(2) + i;
    printf("FAIL: no import %s in the table\n", name); exit(2);
}
/* the arguments, dwords as pushed (a double is two); returns eax */
static uint32_t call_n(const char *name, int n, const uint32_t *a){
    CPU *c = &g_cpu;
    uint32_t base = STACK_TOP - 0x100u;
    for (uint32_t k = 0; k < 8; k++) MR32(base + 4u * k) = 0xC0DE0000u + k;
    c->esp = base;
    for (int k = n; k-- > 0; ) PUSH32(a[k]);
    uint32_t args = c->esp;
    PUSH32(RET_ADDR);
    g_unimpl[0] = 0;
    win32_call(c, idx_of(name));
    int ok = 1;
    for (uint32_t k = 0; k < 8; k++) ok &= MR32(base + 4u * k) == 0xC0DE0000u + k;
    CHECK(c->esp == args, "%s: esp 0x%08x after the call, 0x%08x expected (cdecl: the caller pops)", name, c->esp, args);
    CHECK(c->eip == RET_ADDR, "%s: return address 0x%08x", name, c->eip);
    CHECK(ok, "%s: wrote into the caller's frame", name);
    CHECK(!g_unimpl[0], "%s: not implemented (\"%s\")", name, g_unimpl);
    return c->eax;
}
static uint32_t call(const char *name, ...){
    uint32_t a[12]; int n = 0;
    va_list ap; va_start(ap, name);
    for (uint32_t v; n < 12 && (v = va_arg(ap, uint32_t)) != 0xFEEDFACEu; ) a[n++] = v;
    va_end(ap);
    return call_n(name, n, a);
}
#define END 0xFEEDFACEu
static uint32_t lo(double d){ uint64_t b; memcpy(&b, &d, 8); return (uint32_t)b; }
static uint32_t hi(double d){ uint64_t b; memcpy(&b, &d, 8); return (uint32_t)(b >> 32); }
static double st0(void){ return g_cpu.st[g_cpu.top & 7]; }
static uint64_t bits(double d){ uint64_t b; memcpy(&b, &d, 8); return b; }
static uint32_t g_strn;
static uint32_t gs(const char *s){
    uint32_t p = STR + 512u * (g_strn++ % 256u);
    snprintf((char *)(g_mem + p), 512, "%s", s);
    return p;
}
static uint32_t gws(const uint16_t *w){
    uint32_t p = STR + 512u * (g_strn++ % 256u), k = 0;
    do MR16(p + 2u * k) = w[k]; while (w[k++]);
    return p;
}
static const char *gstr(uint32_t p){ return (const char *)(g_mem + p); }
static void tr(const char *fmt, ...){
    char b[700]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("T %s\n", b);
}

/* ---- _vsnprintf with guest arguments: a dword list at a guest address ---- */
static uint32_t g_va;
static void va_reset(void){ g_va = BUF + 0x80000u; }
static uint32_t va_i(uint32_t v){ MR32(g_va) = v; g_va += 4; return v; }
static void va_d(double d){ va_i(lo(d)); va_i(hi(d)); }
static void fmt_is(const char *fmt, const char *want, uint32_t cap){
    uint32_t out = BUF + 0x90000u, args = BUF + 0x80000u;
    memset(g_mem + out, 0x55, 600);
    int r = (int)call("_vsnprintf", out, cap, gs(fmt), args, END);
    tr("_vsnprintf(\"%s\") -> %d \"%s\"", fmt, r, r >= 0 && (uint32_t)r < cap ? gstr(out) : "");
    CHECK(r == (int)strlen(want) && !strcmp(gstr(out), want), "_vsnprintf(\"%s\"): \"%s\" (%d), wanted \"%s\"", fmt, gstr(out), r, want);
}

static void crt_tests(void){
    /* ---- memory: the DLL's own heap, in its part of the modheap region ---- */
    const ModLink *l = module_link(2);
    uint32_t hlo = l->heap_base, hhi = l->heap_base + l->heap_size * (uint32_t)l->heap_n;
    uint32_t a = call("malloc", 100u, END), b = call("malloc", 0u, END), z = call("calloc", 10u, 12u, END);
    tr("malloc(100) 0x%08x, malloc(0) 0x%08x, calloc(10, 12) 0x%08x", a, b, z);
    CHECK(a >= hlo && a + 100 <= hhi && b >= hlo && b < hhi && b != a, "blocks in the oracle's heap 0x%08x-0x%08x", hlo, hhi);
    int zero = 1; for (uint32_t i = 0; i < 120; i++) zero &= MR8(z + i) == 0;
    CHECK(z && zero, "calloc's block is zeroed");
    CHECK(call("calloc", 0x10000u, 0x10000u, END) == 0, "calloc(65536, 65536): too large, NULL");
    CHECK(call("malloc", 0xFFFFFFF0u, END) == 0, "malloc(0xFFFFFFF0): too large, NULL");
    for (uint32_t i = 0; i < 100; i++) MR8(a + i) = (uint8_t)i;
    uint32_t a2 = call("realloc", a, 5000u, END);
    int same = 1; for (uint32_t i = 0; i < 100; i++) same &= MR8(a2 + i) == (uint8_t)i;
    CHECK(a2 && same, "realloc keeps the bytes (0x%08x -> 0x%08x)", a, a2);
    uint32_t a3 = call("realloc", 0u, 64u, END);
    CHECK(a3 >= hlo && a3 < hhi, "realloc(NULL, 64) is malloc");
    CHECK(call("realloc", a3, 0u, END) == 0, "realloc(p, 0) frees and gives NULL");
    call("free", a2, END); call("free", 0u, END); call("free", b, END);
    uint32_t n1 = call("??2@YAPAXI@Z", 32u, END);
    CHECK(n1 >= hlo && n1 < hhi, "operator new 0x%08x", n1);
    call("??3@YAXPAX@Z", n1, END);
    uint32_t d = call("_strdup", gs("D3DX effect"), END);
    CHECK(d >= hlo && d < hhi && !strcmp(gstr(d), "D3DX effect"), "_strdup");
    CHECK(call("_strdup", 0u, END) == 0, "_strdup(NULL)");
    call("free", d, END);
    call("free", 0x01000040u, END);                   /* not this DLL's: refused, logged */

    /* ---- strings and characters ---- */
    uint32_t s = gs("abcdefgh");
    CHECK(call("memmove", s + 2, s, 5u, END) == s + 2 && !strcmp(gstr(s), "ababcdeh"), "memmove up: %s", gstr(s));
    s = gs("abcdefgh");
    CHECK(call("memmove", s, s + 3, 5u, END) == s && !strcmp(gstr(s), "defghfgh"), "memmove down: %s", gstr(s));
    s = gs("effect.fx");
    CHECK(call("strchr", s, (uint32_t)'.', END) == s + 6, "strchr '.'");
    CHECK(call("strchr", s, 0u, END) == s + 9, "strchr finds the terminator");
    CHECK(call("strchr", s, (uint32_t)'z', END) == 0, "strchr not there");
    CHECK(call("strchr", gs("a\xe9z"), 0xFFFFFFE9u, END) != 0, "strchr's char is the low byte");
    CHECK(call("_stricmp", gs("WorldViewProj"), gs("worldviewproj"), END) == 0, "_stricmp equal without case");
    CHECK((int32_t)call("_stricmp", gs("abc"), gs("ABD"), END) < 0, "_stricmp abc < ABD");
    CHECK((int32_t)call("_stricmp", gs("b"), gs("A"), END) > 0, "_stricmp b > A");
    CHECK((int32_t)call("_stricmp", gs("_"), gs("a"), END) < 0, "_stricmp '_' < 'a' (lower case compared)");
    CHECK((int32_t)call("_stricmp", gs("ab"), gs("abc"), END) < 0, "_stricmp a prefix is less");
    static const uint16_t w1[] = { 'F', 'x', 0xE9, 0 }, w2[] = { 'O', 0x3B1, 'k', 0 };
    CHECK(call("wcslen", gws(w1), END) == 3, "wcslen");
    uint32_t mb = BUF + 0x100u; memset(g_mem + mb, 0x55, 16);
    CHECK(call("wcstombs", 0u, gws(w1), 0u, END) == 3, "wcstombs(NULL): the length");
    CHECK(call("wcstombs", mb, gws(w1), 16u, END) == 3 && !memcmp(g_mem + mb, "Fx\xe9", 4), "wcstombs copies with the terminator");
    memset(g_mem + mb, 0x55, 16);
    CHECK(call("wcstombs", mb, gws(w1), 2u, END) == 2 && !memcmp(g_mem + mb, "Fx\x55", 3), "wcstombs stops at n, no terminator");
    CHECK(call("wcstombs", mb, gws(w2), 16u, END) == 0xFFFFFFFFu, "wcstombs: a character the C locale has not, -1");
    struct { const char *f; uint32_t ch, want; } ct[] = {
        { "isalpha", 'A', 0x101 }, { "isalpha", 'z', 0x102 }, { "isalpha", '5', 0 }, { "isalpha", 0xE9, 0 }, { "isalpha", 0xFFFFFFFFu, 0 },
        { "isalnum", '7', 0x4 }, { "isalnum", 'q', 0x102 }, { "isalnum", '_', 0 }, { "isdigit", '0', 4 }, { "isdigit", 'a', 0 },
        { "isspace", ' ', 8 }, { "isspace", '\t', 8 }, { "isspace", '\r', 8 }, { "isspace", 0xA0, 0 }, { "isspace", 'x', 0 },
        { "isxdigit", 'F', 0x80 }, { "isxdigit", 'f', 0x80 }, { "isxdigit", 'g', 0 }, { "isxdigit", '9', 0x80 },
        { "toupper", 'a', 'A' }, { "toupper", 'A', 'A' }, { "toupper", 0xE9, 0xE9 }, { "toupper", 0xFFFFFFFFu, 0xFFFFFFFFu },
        { "tolower", 'Z', 'z' }, { "tolower", '[', '[' },
        { "iswalpha", 'k', 0x102 }, { "iswalpha", 0xE9, 0x102 }, { "iswalpha", 0xC9, 0x101 }, { "iswalpha", 0x3B1, 0x100 },
        { "iswalpha", 0xD7, 0 }, { "iswdigit", '3', 4 }, { "iswdigit", 0x663, 0 }, { "iswspace", 0xA0, 8 }, { "iswspace", '\n', 8 },
        { "iswpunct", '!', 0x10 }, { "iswpunct", 0xBF, 0x10 }, { "iswpunct", 'a', 0 },
    };
    for (size_t i = 0; i < sizeof ct / sizeof *ct; i++){
        uint32_t r = call(ct[i].f, ct[i].ch, END);
        CHECK(r == ct[i].want, "%s(0x%x) -> 0x%x, wanted 0x%x", ct[i].f, ct[i].ch, r, ct[i].want);
    }

    /* ---- numbers from and to text ---- */
    struct { const char *s; int32_t v; } at[] = { { "  -123abc", -123 }, { "+7", 7 }, { "\t42", 42 }, { "", 0 }, { "x1", 0 },
        { "2147483648", (int32_t)0x80000000u }, { "4294967297", 1 }, { "- 5", 0 } };
    for (size_t i = 0; i < sizeof at / sizeof *at; i++){
        CHECK((int32_t)call("atoi", gs(at[i].s), END) == at[i].v, "atoi(\"%s\")", at[i].s);
        CHECK((int32_t)call("atol", gs(at[i].s), END) == at[i].v, "atol(\"%s\")", at[i].s);
    }
    uint32_t ub = BUF + 0x200u;
    CHECK(call("_ultoa", 255u, ub, 16u, END) == ub && !strcmp(gstr(ub), "ff"), "_ultoa(255, 16) %s", gstr(ub));
    call("_ultoa", 5u, ub, 2u, END);  CHECK(!strcmp(gstr(ub), "101"), "_ultoa(5, 2) %s", gstr(ub));
    call("_ultoa", 0xFFFFFFFFu, ub, 10u, END); CHECK(!strcmp(gstr(ub), "4294967295"), "_ultoa(max, 10) %s", gstr(ub));
    call("_ultoa", 35u, ub, 36u, END); CHECK(!strcmp(gstr(ub), "z"), "_ultoa(35, 36) %s", gstr(ub));
    call("_ultoa", 0u, ub, 8u, END); CHECK(!strcmp(gstr(ub), "0"), "_ultoa(0, 8) %s", gstr(ub));
    /* Microsoft's rand: 41, 18467, 6334, 26500, 19169 from the seed 1 (also before any srand) */
    static const uint32_t rs[5] = { 41, 18467, 6334, 26500, 19169 };
    for (int i = 0; i < 5; i++) CHECK(call("rand", END) == rs[i], "rand #%d, no srand", i);
    call("srand", 1u, END);
    for (int i = 0; i < 5; i++) CHECK(call("rand", END) == rs[i], "rand #%d after srand(1)", i);
    call("srand", 12345u, END);
    { uint32_t x = 12345u, ok = 1; for (int i = 0; i < 100; i++){ x = x * 214013u + 2531011u; ok &= call("rand", END) == ((x >> 16) & 0x7FFFu); }
      CHECK(ok, "rand after srand(12345): 100 values"); }
    struct { const char *s; double v; } af[] = { { "  3.25e2xyz", 325.0 }, { "1.5D3", 1500.0 }, { "-.5", -0.5 }, { "inf", 0.0 },
        { "0x10", 0.0 }, { "1e", 1.0 }, { "7.", 7.0 }, { ".", 0.0 }, { "2.5e-3", 0.0025 }, { "1e400", INFINITY }, { "-0", -0.0 } };
    for (size_t i = 0; i < sizeof af / sizeof *af; i++){
        int32_t t0 = g_cpu.top;
        call("atof", gs(af[i].s), END);
        CHECK(g_cpu.top == ((t0 - 1) & 7) && bits(st0()) == bits(af[i].v), "atof(\"%s\") -> %.17g in ST(0)", af[i].s, st0());
    }

    /* ---- doubles: arguments as two dwords, the result in ST(0) ---- */
    struct { const char *f; double x, want; } dx[] = { { "floor", -2.5, -3.0 }, { "floor", 2.5, 2.0 }, { "floor", -0.0, -0.0 },
        { "ceil", -2.5, -2.0 }, { "ceil", 2.1, 3.0 }, { "ceil", -0.5, -0.0 } };
    for (size_t i = 0; i < sizeof dx / sizeof *dx; i++){
        call(dx[i].f, lo(dx[i].x), hi(dx[i].x), END);
        CHECK(bits(st0()) == bits(dx[i].want), "%s(%g) -> %g", dx[i].f, dx[i].x, st0());
    }
    call("ldexp", lo(3.0), hi(3.0), 4u, END); CHECK(st0() == 48.0, "ldexp(3, 4)");
    call("ldexp", lo(1.0), hi(1.0), (uint32_t)-1100, END); CHECK(st0() == 0.0, "ldexp(1, -1100): underflow to 0");
    uint32_t ep = BUF + 0x300u;
    call("frexp", lo(48.0), hi(48.0), ep, END); CHECK(st0() == 0.75 && MR32(ep) == 6, "frexp(48) 0.75 * 2^6");
    call("modf", lo(-3.75), hi(-3.75), ep, END);
    CHECK(st0() == -0.75 && MRF64(ep) == -3.0, "modf(-3.75) -0.75, -3");
    struct { double x; uint32_t cls; } fc[] = { { NAN, 2 }, { INFINITY, 0x200 }, { -INFINITY, 4 }, { -1.0, 8 }, { -1e-310, 0x10 },
        { -0.0, 0x20 }, { 0.0, 0x40 }, { 1e-310, 0x80 }, { 1.0, 0x100 } };
    for (size_t i = 0; i < sizeof fc / sizeof *fc; i++)
        CHECK(call("_fpclass", lo(fc[i].x), hi(fc[i].x), END) == fc[i].cls, "_fpclass(%g)", fc[i].x);
    { uint64_t sn = 0x7FF0000000000001ull; double v; memcpy(&v, &sn, 8);
      CHECK(call("_fpclass", lo(v), hi(v), END) == 1, "_fpclass(a signalling NaN)"); }
    CHECK(call("_isnan", lo(NAN), hi(NAN), END) && !call("_isnan", lo(1.0), hi(1.0), END), "_isnan");
    CHECK(call("_finite", lo(5.0), hi(5.0), END) && !call("_finite", lo(INFINITY), hi(INFINITY), END)
          && !call("_finite", lo(NAN), hi(NAN), END), "_finite");

    /* ---- the x87 helpers: ST(0) (and ST(1)) in, ST(0) out, at the caller's precision control ---- */
    g_cpu.fcw = 0x027F; g_cpu.top = 0;
    { CPU *c = &g_cpu;
      FPUSH(2.0); call("_CIsqrt", END);
      CHECK(st0() == sqrt(2.0) && c->top == 7, "_CIsqrt(2) at PC 53: %.17g", st0());
      c->fcw = 0x007F; ST(0) = 2.0; call("_CIsqrt", END);
      CHECK(st0() == (double)sqrtf(2.0f), "_CIsqrt(2) at PC 24: %.17g", st0());
      c->fcw = 0x027F;
      FPUSH(2.0); FPUSH(10.0); call("_CIpow", END);
      CHECK(st0() == 1024.0 && c->top == 6, "_CIpow: ST(1) 2 to ST(0) 10 = %g, one popped (top %d)", st0(), c->top);
      FPUSH(7.5); FPUSH(2.0); call("_CIfmod", END);
      CHECK(st0() == 1.5 && c->top == 5, "_CIfmod(7.5, 2) = %g (top %d)", st0(), c->top);
      FPUSH(2.0); call("_CIasin", END);
      CHECK(bits(st0()) == 0xFFF8000000000000ull, "_CIasin(2): the x87's indefinite, 0x%016llx", (unsigned long long)bits(st0()));
      FPUSH(1.0); call("_CIexp", END); CHECK(fabs(st0() - 2.718281828459045) < 1e-15, "_CIexp(1)");
      FPUSH(0.0); call("_CIcosh", END); CHECK(st0() == 1.0, "_CIcosh(0)");
      FPUSH(0.0); call("_CIsinh", END); CHECK(st0() == 0.0, "_CIsinh(0)");
      FPUSH(100.0); call("_CItanh", END); CHECK(st0() == 1.0, "_CItanh(100)");
      FPUSH(1.0); call("_CIacos", END); CHECK(st0() == 0.0, "_CIacos(1)");
      FPUSH(NAN); call("_CIexp", END); CHECK(isnan(st0()) && bits(st0()) != 0xFFF8000000000000ull, "_CIexp(NaN) passes the NaN on");
    }

    /* ---- _controlfp: msvcrt's abstract word against the x87's ---- */
    g_cpu.fcw = 0x027F;
    CHECK(call("_controlfp", 0u, 0u, END) == 0x0009001Fu && g_cpu.fcw == 0x027F, "_controlfp(0, 0) at 0x027f: 0x%x", call("_controlfp", 0u, 0u, END));
    CHECK(call("_controlfp", 0x20000u, 0x30000u, END) == 0x000A001Fu && g_cpu.fcw == 0x007F, "_PC_24: fcw 0x%04x", g_cpu.fcw);
    CHECK(call("_controlfp", 0x00000u, 0x30000u, END) == 0x0008001Fu && g_cpu.fcw == 0x037F, "_PC_64: fcw 0x%04x", g_cpu.fcw);
    CHECK(call("_controlfp", 0x300u, 0x300u, END) == 0x0008031Fu && g_cpu.fcw == 0x0F7F, "_RC_CHOP: fcw 0x%04x", g_cpu.fcw);
    CHECK(call("_controlfp", 0x100u, 0x300u, END) == 0x0008011Fu && g_cpu.fcw == 0x077F, "_RC_DOWN: fcw 0x%04x", g_cpu.fcw);
    CHECK(call("_controlfp", 0u, 0x80000u, END) == 0x0008011Fu && g_cpu.fcw == 0x077F, "_EM_DENORMAL is not _controlfp's: fcw 0x%04x", g_cpu.fcw);
    CHECK(call("_controlfp", 0x10010u, 0x3031Fu, END) == 0x00090010u && g_cpu.fcw == 0x0243, "exceptions unmasked: fcw 0x%04x", g_cpu.fcw);
    CHECK(call("_controlfp", 0x40000u, 0x40000u, END) == 0x000D0010u && g_cpu.fcw == 0x1243, "_IC_AFFINE: fcw 0x%04x", g_cpu.fcw);
    g_cpu.fcw = 0x027F;

    /* ---- _vsnprintf ---- */
    va_reset(); va_i(42); va_i((uint32_t)-7); va_i(3000000000u); fmt_is("%d %i %u", "42 -7 3000000000", 64);
    va_reset(); va_i(42); va_i(42); va_i(42); va_i((uint32_t)-42); fmt_is("%5d|%-5d|%05d|%+d", "   42|42   |00042|-42", 64);
    va_reset(); va_i(255); va_i(255); va_i(255); va_i(8); va_i(8); fmt_is("%x %X %#x %o %#o", "ff FF 0xff 10 010", 64);
    va_reset(); va_i(0x1234); fmt_is("%p", "00001234", 64);
    va_reset(); va_i(0); va_i(gs("effect")); va_i('Z'); fmt_is("%s|%.3s|%c|%%", "(null)|eff|Z|%", 64);
    va_reset(); va_i(5); va_i(gs("ab")); fmt_is("[%*s]", "[   ab]", 64);
    va_reset(); va_i(0xFFFFFFFFu); va_i(0x7FFFFFFFu); fmt_is("%I64d", "9223372036854775807", 64);
    va_reset(); va_i(0x18000u); fmt_is("%hd", "-32768", 64);
    va_reset(); va_i(3); fmt_is("%.5d", "00003", 64);
    va_reset(); va_d(3.14159); fmt_is("%f", "3.141590", 64);
    va_reset(); va_d(12345.678); fmt_is("%e", "1.234568e+004", 64);
    va_reset(); va_d(12345.678); fmt_is("%.2E", "1.23E+004", 64);
    va_reset(); va_d(0.0001); fmt_is("%g", "0.0001", 64);
    va_reset(); va_d(1e-5); fmt_is("%g", "1e-005", 64);
    va_reset(); va_d(1e20); fmt_is("%G", "1E+020", 64);
    va_reset(); va_d(-1.5); fmt_is("%8.2f|", "   -1.50|", 64);
    va_reset(); va_d(1.5); fmt_is("%08.3f", "0001.500", 64);
    va_reset(); va_d(INFINITY); fmt_is("%f", "1.#INF00", 64);
    va_reset(); va_d(-INFINITY); fmt_is("%e", "-1.#INF00e+000", 64);
    va_reset(); va_d(NAN); fmt_is("%f", "1.#QNAN0", 64);
    va_reset(); { uint64_t ind = 0xFFF8000000000000ull; double v; memcpy(&v, &ind, 8); va_d(v); } fmt_is("%f", "-1.#IND00", 64);
    va_reset(); va_d(INFINITY); fmt_is("%g", "1.#INF", 64);
    va_reset(); va_i(BUF + 0x400u); fmt_is("abc%n", "abc", 64);
    CHECK(MR32(BUF + 0x400u) == 3, "%%n stored 3");
    va_reset(); fmt_is("%y!", "%y!", 64);
    /* the size rule: exactly full gives the count and no terminator; more gives -1 */
    { uint32_t out = BUF + 0x90000u, args = BUF + 0x80000u;
      memset(g_mem + out, 0x55, 16); va_reset();
      int r = (int)call("_vsnprintf", out, 5u, gs("hello"), args, END);
      CHECK(r == 5 && !memcmp(g_mem + out, "hello\x55", 6), "_vsnprintf exactly full: %d, no terminator", r);
      memset(g_mem + out, 0x55, 16);
      r = (int)call("_vsnprintf", out, 5u, gs("hello!"), args, END);
      CHECK(r == -1 && !memcmp(g_mem + out, "hello\x55", 6), "_vsnprintf too long: %d, 5 written", r);
      r = (int)call("_vsnprintf", out, 0u, gs("x"), args, END);
      CHECK(r == -1 && MR8(out) == 'h', "_vsnprintf(count 0): -1, nothing written"); }

    /* ---- sscanf: the pointers follow the format on the stack ---- */
    { uint32_t pi = BUF + 0x500u, ps = BUF + 0x510u, pf = BUF + 0x560u, px = BUF + 0x570u, pd = BUF + 0x580u, pn = BUF + 0x590u;
      int r = (int)call("sscanf", gs("12 abc 3.5 0x1f"), gs("%d %s %f %x"), pi, ps, pf, px, END);
      CHECK(r == 4 && MR32(pi) == 12 && !strcmp(gstr(ps), "abc") && MRF32(pf) == 3.5f && MR32(px) == 0x1f,
            "sscanf four fields: %d %u \"%s\" %g 0x%x", r, MR32(pi), gstr(ps), (double)MRF32(pf), MR32(px));
      r = (int)call("sscanf", gs("12345"), gs("%3d%d"), pi, px, END);
      CHECK(r == 2 && MR32(pi) == 123 && MR32(px) == 45, "sscanf widths");
      r = (int)call("sscanf", gs("vs_1_1 rest"), gs("vs_%d_%d%n"), pi, px, pn, END);
      CHECK(r == 2 && MR32(pi) == 1 && MR32(px) == 1 && MR32(pn) == 6, "sscanf literals and %%n (%d, n %u)", r, MR32(pn));
      r = (int)call("sscanf", gs("abcxyz"), gs("%[a-c]%s"), ps, pd, END);
      CHECK(r == 2 && !strcmp(gstr(ps), "abc") && !strcmp(gstr(pd), "xyz"), "sscanf scan set");
      r = (int)call("sscanf", gs("x: 7"), gs("%*s %d"), pi, END);
      CHECK(r == 1 && MR32(pi) == 7, "sscanf %%*s skips");
      r = (int)call("sscanf", gs("2.5e3"), gs("%lf"), pd, END);
      CHECK(r == 1 && MRF64(pd) == 2500.0, "sscanf %%lf");
      r = (int)call("sscanf", gs(""), gs("%d"), pi, END);
      CHECK(r == -1, "sscanf on nothing: EOF (%d)", r);
      r = (int)call("sscanf", gs("5 x"), gs("%d %d"), pi, px, END);
      CHECK(r == 1, "sscanf stops at a failure: %d", r);
      r = (int)call("sscanf", gs("-0x10 077 -5"), gs("%i %i %u"), pi, px, pd, END);
      CHECK(r == 3 && (int32_t)MR32(pi) == -16 && MR32(px) == 63 && MR32(pd) == (uint32_t)-5, "sscanf %%i bases, %%u's sign");
      r = (int)call("sscanf", gs("ab"), gs("%c%c"), ps, ps + 1, END);
      CHECK(r == 2 && MR8(ps) == 'a' && MR8(ps + 1) == 'b', "sscanf %%c");
      r = (int)call("sscanf", gs("70000"), gs("%hd"), pi, END);
      CHECK(r == 1 && MR16(pi) == (uint16_t)70000, "sscanf %%hd stores 16 bits"); }

    /* ---- time, locale, temporary names ---- */
    uint32_t tb = BUF + 0x600u;
    CHECK(call("_strtime", tb, END) == tb && !strcmp(gstr(tb), "12:00:00"), "_strtime: GetLocalTime's time, %s", gstr(tb));
    CHECK(call("_strdate", tb, END) == tb && !strcmp(gstr(tb), "08/24/26"), "_strdate: GetLocalTime's date, %s", gstr(tb));
    uint32_t lc = call("setlocale", 0u, 0u, END);
    CHECK(lc && !strcmp(gstr(lc), "C"), "setlocale(LC_ALL, NULL): \"C\"");
    CHECK(call("setlocale", 4u, gs("C"), END) == lc, "setlocale(LC_NUMERIC, \"C\")");
    CHECK(call("setlocale", 0u, gs(""), END) == lc, "setlocale(LC_ALL, \"\"): \"C\" here");
    CHECK(call("setlocale", 0u, gs("German"), END) == 0, "setlocale(LC_ALL, \"German\"): NULL");
    CHECK(call("setlocale", 9u, 0u, END) == 0, "setlocale(9): no such category");
    CHECK(call("_tempnam", gs("C:\\TEMP"), gs("fx"), END) == 0, "_tempnam: NULL");

    /* ---- qsort, with a guest comparator: always two of the array's elements ---- */
    for (int round = 0; round < 3; round++){
        uint32_t num = round == 0 ? 10u : round == 1 ? 1000u : 1u;
        g_arr = BUF + 0x1000u; g_arr_end = g_arr + 4u * num; g_cmp_calls = g_cmp_outside = 0;
        uint32_t x = 7u + (uint32_t)round, sum = 0;
        for (uint32_t i = 0; i < num; i++){ x = x * 1103515245u + 12345u; MR32(g_arr + 4u * i) = (x >> 8) % 5000u; sum += MR32(g_arr + 4u * i); }
        CPU before = g_cpu;
        call("qsort", g_arr, num, 4u, FN_CMP, END);
        int sorted = 1; uint32_t sum2 = 0;
        for (uint32_t i = 0; i < num; i++){ sum2 += MR32(g_arr + 4u * i); if (i) sorted &= MR32(g_arr + 4u * i - 4u) <= MR32(g_arr + 4u * i); }
        tr("qsort(%u ints): %u comparator calls, %u outside the array", num, g_cmp_calls, g_cmp_outside);
        CHECK(sorted && sum == sum2 && !g_cmp_outside, "qsort(%u ints) sorted, same elements, comparator given only the array's", num);
        CHECK(g_cpu.ebx == before.ebx && g_cpu.esi == before.esi && g_cpu.edi == before.edi && g_cpu.ebp == before.ebp,
              "qsort: the caller's registers kept");
    }
    CHECK(call("qsort", BUF, 5u, 0u, FN_CMP, END) == 0, "qsort with width 0 does nothing");
    /* ties: a line for each of the first 8 comparators that tie (FN_CMP's above among them), then one line
     * for the rest; N_CMPS more comparators, each sorting five equal ints twice */
    for (uint32_t k = 0; k < N_CMPS; k++)
        for (int twice = 0; twice < 2; twice++){
            g_arr = BUF + 0x1000u; g_arr_end = g_arr + 20u; g_cmp_outside = 0;
            for (uint32_t i = 0; i < 5u; i++) MR32(g_arr + 4u * i) = 42u;
            call("qsort", g_arr, 5u, 4u, FN_CMPS + 16u * k, END);
            CHECK(!g_cmp_outside, "qsort with comparator %u: given only the array's elements", k + 2u);
        }

    /* ---- _initterm: the table's functions in order, empty slots skipped; the control word they set stays ---- */
    { uint32_t t = BUF + 0x700u; MR32(t) = FN_INIT1; MR32(t + 4) = 0; MR32(t + 8) = FN_INIT2;
      g_init_order = 0; g_cpu.fcw = 0x027F;
      call("_initterm", t, t + 12u, END);
      CHECK(g_init_order == 12 && g_cpu.fcw == 0x007F, "_initterm ran 1 then 2 (%u), fcw 0x%04x", g_init_order, g_cpu.fcw);
      g_init_order = 0; call("_initterm", t, t, END);
      CHECK(g_init_order == 0, "_initterm of an empty range");
      g_cpu.fcw = 0x027F; }

    /* ---- _setjmp3: 0, and the buffer msvcrt fills ---- */
    { uint32_t jb = BUF + 0x800u; MR32(TEB_MAIN) = 0xFFFFFFFFu;
      g_cpu.ebp = 0x11; g_cpu.ebx = 0x22; g_cpu.edi = 0x33; g_cpu.esi = 0x44;
      uint32_t r = call("_setjmp3", jb, 0u, END);
      CHECK(r == 0 && MR32(jb) == 0x11 && MR32(jb + 4) == 0x22 && MR32(jb + 8) == 0x33 && MR32(jb + 12) == 0x44
            && MR32(jb + 20) == RET_ADDR && MR32(jb + 24) == 0xFFFFFFFFu && MR32(jb + 28) == 0xFFFFFFFFu && MR32(jb + 32) == 0x56433230u,
            "_setjmp3: 0, registers, return address, no SEH frame, 'VC20'"); }

    /* ---- exit: as ExitProcess, back to the runtime's exit point ---- */
    { volatile int back = 0;
      if (setjmp(g_exit_jmp) == 0){ g_exit_armed = 1; call("exit", 7u, END); }
      else back = 1;
      g_exit_armed = 0;
      CHECK(back && g_exit_code == 7, "exit(7) left through the exit point, code %d", g_exit_code); }

    /* ---- the variable: _adjust_fdiv, bound in the DLL's IAT (module.h bind) ---- */
    { const ImpEnt v = { 0, "msvcrt.dll", "_adjust_fdiv", 0 }, f = { 0, "msvcrt.dll", "malloc", 0 }, k = { 0, "KERNEL32.dll", "_adjust_fdiv", 0 };
      uint32_t va = l->bind ? l->bind(&v) : 0;
      CHECK(va && MR32(va) == 0 && l->bind(&v) == va, "_adjust_fdiv bound to 0x%08x, holding 0", va);
      CHECK(l->bind(&f) == 0 && l->bind(&k) == 0, "functions and other DLLs' names are not bound");
      CHECK(!strcmp(l->file_env, "NFSMW_D3DX"), "the oracle's file comes from NFSMW_D3DX");
      call("_adjust_fdiv", END); }
}

int main(int argc, char **argv){
    const char *mode = argc > 1 ? argv[1] : "crt";
    memset(g_cbits, 0xFF, sizeof g_cbits);
    win32_init();
    g_cpu.fs_base = TEB_MAIN; g_cpu.fcw = 0x027F;
    CHECK(module_count() == 2 && !strcmp(module_link(2)->info->name, "d3dx9_26.dll"), "Most Wanted's table lists the oracle");
    if (!strcmp(mode, "crt")) crt_tests();
    else if (!strcmp(mode, "stop") && argc > 2){
        uint32_t jb = BUF + 0x800u; MR32(jb + 20) = 0x00DABCDEu;
        if (!strcmp(argv[2], "longjmp")) call("longjmp", jb, 1u, END);
        else call(argv[2], END);
        printf("FAIL %s returned\n", argv[2]); fail++;
    } else { printf("usage: msvcrt_unit crt | stop <import>\n"); return 2; }
    fflush(stderr);
    printf("msvcrt_unit: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
