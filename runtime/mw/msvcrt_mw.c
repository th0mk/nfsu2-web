/* Most Wanted: msvcrt.dll for the lifted DLLs that import it.  That is the
 * D3DX oracle, d3dx9_26.dll (dev builds only, PLAN D-09): speed.exe and
 * server.dll link their C runtime statically.  game_mw.c routes every
 * msvcrt.dll import here (game.h win32_misc) and binds its one variable,
 * _adjust_fdiv, through the oracle's module entry (module.h bind).
 *
 * Every import is cdecl: the caller pops the arguments (argcount's KNOWN
 * gives 0), so the answers read them at esp and leave esp alone.  Results are
 * in eax, or in the x87's ST(0) for the double ones; the _CI functions also
 * take their arguments there (ST(0), and ST(1) for _CIpow and _CIfmod).  The
 * code is our own, from the C standard and Microsoft's documentation of
 * msvcrt; the locale is "C" throughout (the only one setlocale gives).
 *
 * The heap: each lifted DLL's msvcrt heap is made at its first allocation, in
 * that DLL's own part of the modheap region (game.h modules, modheap.c), as
 * msvcrt keeps a heap of its own on Windows.
 *
 * Stops, each logged with the caller: terminate and _purecall, which end the
 * process on Windows too (abort's exit code 3); the exception handlers
 * _except_handler3 and __CxxFrameHandler, which only the system's exception
 * dispatch calls and the runtime has none (win32.c RaiseException); longjmp,
 * which would have to leave lifted C functions in the middle (exit code 5,
 * as RaiseException's stop). */
#include "cpu.h"
#include "module.h"
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

uint32_t modheap_create(int mod); int modheap_free(uint32_t h, uint32_t u);
uint32_t modheap_alloc(uint32_t h, uint32_t n, uint32_t flags);
uint32_t modheap_realloc(uint32_t h, uint32_t u, uint32_t n, uint32_t flags);
uint32_t gheap_alloc(int heap, uint32_t n, int zero);
int win32_caller_module(void);
void guest_backtrace(CPU *c, int n);
void dispatch_indirect(CPU *c, uint32_t addr);
extern jmp_buf g_exit_jmp; extern int g_exit_code, g_exit_armed, g_guest_exited;
extern void (*g_report_fn)(void);

static uint32_t A(CPU *c, int i){ return MR32(c->esp + 4u * (uint32_t)i); }
static double AD(CPU *c, int i){ uint64_t b = (uint64_t)A(c, i) | (uint64_t)A(c, i + 1) << 32; double d; memcpy(&d, &b, 8); return d; }
static void put_d(uint32_t p, double d){ uint64_t b; memcpy(&b, &d, 8); MR32(p) = (uint32_t)b; MR32(p + 4u) = (uint32_t)(b >> 32); }
static const char *S(uint32_t p){ return (const char *)(g_mem + p); }
static _Thread_local uint32_t g_crt_rand = 1;        /* rand's state, per thread as msvcrt keeps it: 1 until srand */

/* ---- the names, in the order of the oracle's import table ---- */
#define NAMES(X) X(except_handler3, "_except_handler3") X(terminate, "?terminate@@YAXXZ") X(adjust_fdiv, "_adjust_fdiv") \
    X(initterm, "_initterm") X(srand, "srand") X(tempnam, "_tempnam") X(exit, "exit") X(atol, "atol") X(ultoa, "_ultoa") \
    X(wcstombs, "wcstombs") X(rand, "rand") X(isxdigit, "isxdigit") X(atof, "atof") X(CIcosh, "_CIcosh") X(CIexp, "_CIexp") \
    X(CIsinh, "_CIsinh") X(CItanh, "_CItanh") X(fpclass, "_fpclass") X(strchr, "strchr") X(isnan, "_isnan") \
    X(isalnum, "isalnum") X(isalpha, "isalpha") X(toupper, "toupper") X(tolower, "tolower") X(atoi, "atoi") X(floor, "floor") \
    X(strtime, "_strtime") X(strdate, "_strdate") X(sscanf, "sscanf") X(isspace, "isspace") X(isdigit, "isdigit") \
    X(setjmp3, "_setjmp3") X(longjmp, "longjmp") X(ldexp, "ldexp") X(frexp, "frexp") X(wcslen, "wcslen") X(calloc, "calloc") \
    X(realloc, "realloc") X(malloc, "malloc") X(CIsqrt, "_CIsqrt") X(setlocale, "setlocale") X(strdup, "_strdup") X(free, "free") \
    X(ceil, "ceil") X(controlfp, "_controlfp") X(vsnprintf, "_vsnprintf") X(CIasin, "_CIasin") X(cxxframe, "__CxxFrameHandler") \
    X(finite, "_finite") X(CIacos, "_CIacos") X(memmove, "memmove") X(qsort, "qsort") X(CIfmod, "_CIfmod") X(purecall, "_purecall") \
    X(stricmp, "_stricmp") X(modf, "modf") X(iswspace, "iswspace") X(iswalpha, "iswalpha") X(iswdigit, "iswdigit") \
    X(iswpunct, "iswpunct") X(CIpow, "_CIpow") X(opnew, "??2@YAPAXI@Z") X(opdelete, "??3@YAXPAX@Z")
#define X_ENUM(id, s) F_##id,
#define X_NAME(id, s) s,
enum { NAMES(X_ENUM) F_COUNT };
static const char *const k_names[F_COUNT] = { NAMES(X_NAME) };
static int fn_id(const char *n){
    for (int i = 0; i < F_COUNT; i++) if (!strcmp(n, k_names[i])) return i;
    return -1;
}

/* ---- the heap of the calling DLL ---- */
#define MAXMOD 8
static uint32_t g_heap[MAXMOD];
static uint32_t crt_heap(void){
    int m = win32_caller_module();
    if (m < 1 || m >= MAXMOD) return 0;
    if (!g_heap[m]) g_heap[m] = modheap_create(m);
    return g_heap[m];
}
#define HEAP_MAXREQ 0xFFFFFFE0u                  /* msvcrt refuses anything larger */
static uint32_t crt_malloc(uint32_t n, int zero){
    uint32_t h = crt_heap();
    if (!h || n > HEAP_MAXREQ) return 0;
    return modheap_alloc(h, n ? n : 1u, zero ? 8u : 0u);       /* malloc(0): a block of its own */
}
static void crt_free(CPU *c, uint32_t p){
    uint32_t h = crt_heap();
    if (p && h && !modheap_free(h, p)){
        static int said = 0;
        if (said++ < 8) fprintf(stderr, "[msvcrt] free(0x%08x): not a block of this DLL's heap (from 0x%08x)\n", p, c->eip);
    }
}

/* ---- a guest function, called as a cdecl caller calls it ----
 * The control word the callee leaves stays (a constructor may set it, as on
 * Windows); every other register is the caller's again. */
static uint32_t crt_call(CPU *c, uint32_t fn, int n, const uint32_t *a){
    CPU save = *c;
    for (int i = n - 1; i >= 0; i--) PUSH32(a[i]);
    PUSH32(0xDEADBEEFu);
    dispatch_indirect(c, fn);
    uint32_t r = c->eax; uint16_t fcw = c->fcw;
    *c = save; c->fcw = fcw;
    return r;
}

static void crt_stop(CPU *c, const char *what, int code){
    fprintf(stderr, "\n*** GUEST msvcrt %s from 0x%08x: %s, exiting (code %d)\n", what, c->eip,
            code == 3 ? "the process ends here on Windows too" : "not supported by the runtime", code);
    guest_backtrace(c, 12);
    if (g_report_fn) g_report_fn();
    fflush(stderr); _exit(code);
}

/* ---- character classes: the C locale's, as Win32's C1_* bits (msvcrt's own masks) ---- */
#define C1_UPPER 0x001u
#define C1_LOWER 0x002u
#define C1_DIGIT 0x004u
#define C1_SPACE 0x008u
#define C1_PUNCT 0x010u
#define C1_CNTRL 0x020u
#define C1_BLANK 0x040u
#define C1_XDIGIT 0x080u
#define C1_ALPHA 0x100u
static uint32_t ctype_c(uint32_t ch){                /* 0..127; nothing above in the C locale */
    if (ch >= 128) return 0;
    if (ch >= 'A' && ch <= 'Z') return C1_UPPER | C1_ALPHA | (ch <= 'F' ? C1_XDIGIT : 0);
    if (ch >= 'a' && ch <= 'z') return C1_LOWER | C1_ALPHA | (ch <= 'f' ? C1_XDIGIT : 0);
    if (ch >= '0' && ch <= '9') return C1_DIGIT | C1_XDIGIT;
    if (ch == ' ') return C1_SPACE | C1_BLANK;
    if (ch == '\t') return C1_SPACE | C1_CNTRL | C1_BLANK;
    if (ch >= 0x0A && ch <= 0x0D) return C1_SPACE | C1_CNTRL;
    if (ch < 0x20 || ch == 0x7F) return C1_CNTRL;
    return C1_PUNCT;
}
/* the wide classes: Latin-1 by its Unicode categories (msvcrt's table for 0..255); above, what
 * GetStringTypeW answers on this runtime (win32.c: a letter) */
static uint32_t ctype_w(uint32_t ch){
    if (ch < 128) return ctype_c(ch);
    if (ch < 0xA0) return C1_CNTRL;
    if (ch == 0xA0) return C1_SPACE | C1_BLANK;
    if (ch == 0xAA || ch == 0xB5 || ch == 0xBA || (ch >= 0xDF && ch <= 0xFF && ch != 0xF7)) return C1_LOWER | C1_ALPHA;
    if (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7) return C1_UPPER | C1_ALPHA;
    if (ch < 0x100) return C1_PUNCT;
    return ch <= 0xFFFFu ? C1_ALPHA : 0;
}
static int is_space(int ch){ return ch == ' ' || (ch >= 0x09 && ch <= 0x0D); }

/* ---- x87 control word: msvcrt's abstract form (_MCW_EM, _MCW_IC, _MCW_RC, _MCW_PC) ---- */
static uint32_t cw_abstract(uint16_t cw){
    uint32_t r = 0;
    if (cw & 0x01) r |= 0x10u;                        /* IM -> _EM_INVALID */
    if (cw & 0x02) r |= 0x80000u;                     /* DM -> _EM_DENORMAL */
    if (cw & 0x04) r |= 0x08u;                        /* ZM -> _EM_ZERODIVIDE */
    if (cw & 0x08) r |= 0x04u;                        /* OM -> _EM_OVERFLOW */
    if (cw & 0x10) r |= 0x02u;                        /* UM -> _EM_UNDERFLOW */
    if (cw & 0x20) r |= 0x01u;                        /* PM -> _EM_INEXACT */
    r |= (uint32_t)((cw >> 10) & 3u) << 8;            /* RC: _RC_NEAR, _RC_DOWN, _RC_UP, _RC_CHOP */
    switch ((cw >> 8) & 3u){ case 0: r |= 0x20000u; break; case 2: r |= 0x10000u; break; default: break; }   /* PC 24, 53, 64 */
    if (cw & 0x1000) r |= 0x40000u;                   /* IC -> _IC_AFFINE */
    return r;
}
static uint16_t cw_x87(uint32_t a, uint16_t cw){
    cw &= (uint16_t)~0x1F3Fu;
    if (a & 0x10u) cw |= 0x01; if (a & 0x80000u) cw |= 0x02; if (a & 0x08u) cw |= 0x04;
    if (a & 0x04u) cw |= 0x08; if (a & 0x02u) cw |= 0x10; if (a & 0x01u) cw |= 0x20;
    cw |= (uint16_t)(((a >> 8) & 3u) << 10);
    cw |= (uint16_t)((a & 0x20000u) ? 0x000u : (a & 0x10000u) ? 0x200u : 0x300u);
    if (a & 0x40000u) cw |= 0x1000;
    return cw;
}

/* ---- numbers from text ---- */
/* atol: whitespace, a sign, digits; wraps around as msvcrt's does */
static uint32_t crt_atol(const char *s){
    while (is_space((unsigned char)*s)) s++;
    int neg = *s == '-'; if (*s == '-' || *s == '+') s++;
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10u + (uint32_t)(*s++ - '0');
    return neg ? 0u - v : v;
}
/* the longest prefix of s msvcrt reads as a floating-point number (sign, digits, a point, digits, an
 * exponent with e, E, d or D: no hex, no infinity, no NaN), copied with a C exponent letter; its length */
static size_t float_text(const char *s, size_t max, char *out, size_t cap){
    size_t i = 0, o = 0, nd = 0;
    #define TAKE(ch) ((void)(o + 1 < cap ? (out[o++] = (char)(ch)) : 0))
    #define DIGIT(k) ((k) < max && s[k] >= '0' && s[k] <= '9')
    if (i < max && (s[i] == '+' || s[i] == '-')){ TAKE(s[i]); i++; }
    while (DIGIT(i)){ TAKE(s[i]); i++; nd++; }
    if (i < max && s[i] == '.'){ TAKE('.'); i++; while (DIGIT(i)){ TAKE(s[i]); i++; nd++; } }
    if (!nd){ out[0] = 0; return 0; }
    if (i < max && (s[i] == 'e' || s[i] == 'E' || s[i] == 'd' || s[i] == 'D')){
        size_t j = i + 1, oo = o;
        TAKE('e');
        if (j < max && (s[j] == '+' || s[j] == '-')){ TAKE(s[j]); j++; }
        if (DIGIT(j)){ while (DIGIT(j)){ TAKE(s[j]); j++; } i = j; }
        else o = oo;                                  /* no digits: the exponent is not part of it */
    }
    #undef TAKE
    #undef DIGIT
    out[o] = 0;
    return i;
}

/* ---- _vsnprintf: printf over guest memory, the arguments at guest va ---- */
typedef struct { uint32_t dst, cap, n; } Out;        /* n: characters produced, written while n < cap */
static void put_c(Out *o, char ch){ if (o->n < o->cap) MR8(o->dst + o->n) = (uint8_t)ch; o->n++; }
static void put_pad(Out *o, char ch, int k){ while (k-- > 0) put_c(o, ch); }
static void put_s(Out *o, const char *s, size_t k){ for (size_t i = 0; i < k; i++) put_c(o, s[i]); }
static void put_field(Out *o, const char *pre, const char *body, size_t blen, int width, int left, int zero){
    int len = (int)(strlen(pre) + blen);
    if (!left && !zero) put_pad(o, ' ', width - len);
    put_s(o, pre, strlen(pre));
    if (!left && zero) put_pad(o, '0', width - len);
    put_s(o, body, blen);
    if (left) put_pad(o, ' ', width - len);
}
/* a double, %e/%f/%g: the C library's digits, the exponent with three digits as msvcrt writes it;
 * infinities and NaNs as msvcrt names them ("1.#INF", "1.#QNAN", "1.#IND" for the x87's indefinite) */
static int fmt_double(char *out, size_t cap, double v, char conv, int prec, int alt){
    if (!isfinite(v)){
        uint64_t b; memcpy(&b, &v, 8);
        const char *tag = isinf(v) ? "INF" : b == 0xFFF8000000000000ull ? "IND" : (b & 0x0008000000000000ull) ? "QNAN" : "SNAN";
        int k = snprintf(out, cap, "1.#%s", tag);
        if (conv != 'g' && conv != 'G') for (int d = (int)strlen(tag) + 1; d < prec && k + 1 < (int)cap; d++) out[k++] = '0';
        out[k] = 0;
        if (conv == 'e' || conv == 'E') k += snprintf(out + k, cap - (size_t)k, "%c+000", conv);
        return k;
    }
    char f[16]; snprintf(f, sizeof f, "%%%s.%d%c", alt ? "#" : "", prec, conv);
    char t[400]; snprintf(t, sizeof t, f, fabs(v));
    char *e = strpbrk(t, "eE");
    if (e && (e[1] == '+' || e[1] == '-')){           /* e+NN -> e+0NN */
        size_t ed = strlen(e + 2);
        if (ed < 3){ memmove(e + 2 + (3 - ed), e + 2, ed + 1); memset(e + 2, '0', 3 - ed); }
    }
    return snprintf(out, cap, "%s", t);
}
static int crt_vsnprintf(uint32_t dst, uint32_t cap, const char *fmt, uint32_t va){
    Out o = { dst, cap, 0 };
    for (const char *p = fmt; *p; p++){
        if (*p != '%'){ put_c(&o, *p); continue; }
        const char *start = p++;
        int left = 0, plus = 0, space = 0, alt = 0, zero = 0, width = 0, prec = -1, size = 0;  /* size: 1 h, 2 l, 3 I64, 4 wide */
        for (;; p++){
            if (*p == '-') left = 1; else if (*p == '+') plus = 1; else if (*p == ' ') space = 1;
            else if (*p == '#') alt = 1; else if (*p == '0') zero = 1; else break;
        }
        if (*p == '*'){ width = (int)MR32(va); va += 4; if (width < 0){ left = 1; width = -width; } p++; }
        else while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        if (*p == '.'){
            p++; prec = 0;
            if (*p == '*'){ prec = (int)MR32(va); va += 4; if (prec < 0) prec = -1; p++; }
            else while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
        }
        if (*p == 'h'){ size = 1; p++; }
        else if (*p == 'l'){ size = 2; p++; if (*p == 'l'){ size = 3; p++; } }
        else if (*p == 'L'){ p++; }
        else if (*p == 'w'){ size = 4; p++; }
        else if (p[0] == 'I' && p[1] == '6' && p[2] == '4'){ size = 3; p += 3; }
        else if (p[0] == 'I' && p[1] == '3' && p[2] == '2'){ p += 3; }
        else if (*p == 'I'){ p++; }
        char conv = *p;
        if (left) zero = 0;
        switch (conv){
        case '%': put_c(&o, '%'); break;
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
            uint64_t v;
            if (size == 3){ v = (uint64_t)MR32(va) | (uint64_t)MR32(va + 4) << 32; va += 8; }
            else { v = MR32(va); va += 4; if (size == 1) v &= 0xFFFFu; }
            int neg = 0;
            if (conv == 'd' || conv == 'i'){
                int64_t s = size == 3 ? (int64_t)v : size == 1 ? (int16_t)v : (int32_t)(uint32_t)v;
                if (s < 0){ neg = 1; v = (uint64_t)0 - (uint64_t)s; } else v = (uint64_t)s;
            }
            if (conv == 'p'){ prec = 8; alt = 0; conv = 'X'; }
            int base = conv == 'o' ? 8 : (conv == 'x' || conv == 'X') ? 16 : 10;
            const char *dig = conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char t[72]; int k = 0;
            for (uint64_t w = v; w; w /= (uint64_t)base) t[k++] = dig[w % (uint64_t)base];
            char body[160]; int bl = 0;
            int want = prec < 0 ? 1 : prec;
            if (prec >= 0) zero = 0;
            if (alt && conv == 'o' && v && want <= k) want = k + 1;
            for (int z = k; z < want && bl < 140; z++) body[bl++] = '0';
            while (k > 0) body[bl++] = t[--k];
            char pre[4] = "";
            if (neg) strcpy(pre, "-"); else if ((conv == 'd' || conv == 'i') && plus) strcpy(pre, "+");
            else if ((conv == 'd' || conv == 'i') && space) strcpy(pre, " ");
            if (alt && v && (conv == 'x' || conv == 'X')) strcpy(pre, conv == 'x' ? "0x" : "0X");
            put_field(&o, pre, body, (size_t)bl, width, left, zero);
            break; }
        case 'c': case 'C': {
            uint32_t ch = MR32(va); va += 4;
            if (conv == 'C' || size == 2 || size == 4) ch = (ch & 0xFFFFu) < 256 ? ch & 0xFFu : '?';
            char b = (char)ch;
            put_field(&o, "", &b, 1, width, left, 0);
            break; }
        case 's': case 'S': {
            uint32_t s = MR32(va); va += 4;
            int wide = conv == 'S' || size == 2 || size == 4;
            char t[1024]; size_t k = 0;
            if (!s){ k = 6; memcpy(t, "(null)", 6); }
            else if (wide){ for (uint32_t q = s; MR16(q) && (prec < 0 || k < (size_t)prec) && k < sizeof t; q += 2){ uint32_t w = MR16(q); t[k++] = w < 256 ? (char)w : '?'; } }
            else { for (uint32_t q = s; MR8(q) && (prec < 0 || k < (size_t)prec) && k < sizeof t; q++) t[k++] = (char)MR8(q); }
            if (!s && prec >= 0 && k > (size_t)prec) k = (size_t)prec;
            put_field(&o, "", t, k, width, left, 0);
            break; }
        case 'e': case 'E': case 'f': case 'g': case 'G': {
            double v; uint64_t b = (uint64_t)MR32(va) | (uint64_t)MR32(va + 4) << 32; memcpy(&v, &b, 8); va += 8;
            char t[420]; int k = fmt_double(t, sizeof t, v, conv, prec < 0 ? 6 : prec, alt);
            char pre[2] = "";
            if (signbit(v)) strcpy(pre, "-"); else if (plus) strcpy(pre, "+"); else if (space) strcpy(pre, " ");
            put_field(&o, pre, t, (size_t)k, width, left, zero && isfinite(v));
            break; }
        case 'n': { uint32_t q = MR32(va); va += 4; if (q){ if (size == 1) MR16(q) = (uint16_t)o.n; else MR32(q) = o.n; } break; }
        default:                                      /* not a conversion: msvcrt writes it as it is */
            put_s(&o, start, (size_t)(p - start) + (*p ? 1 : 0));
            if (!*p) p--;
            break;
        }
    }
    if (o.n < cap){ MR8(dst + o.n) = 0; return (int)o.n; }
    return o.n == cap ? (int)cap : -1;                /* exactly full: no terminator; more: -1 */
}

/* ---- sscanf over guest memory, the pointers from guest ap ---- */
static int crt_sscanf(const char *in, const char *fmt, uint32_t ap){
    size_t i = 0; int got = 0, conv_seen = 0;
    for (const char *p = fmt; *p; p++){
        if (is_space((unsigned char)*p)){ while (is_space((unsigned char)in[i])) i++; continue; }
        if (*p != '%' || p[1] == '%'){
            if (*p == '%') p++;
            if (in[i] != *p) break;                   /* a literal that does not match: the end */
            i++; continue;
        }
        p++;
        int skip = 0, width = 0, size = 0;            /* size: 1 h, 2 l, 3 I64 */
        if (*p == '*'){ skip = 1; p++; }
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        if (*p == 'h'){ size = 1; p++; }
        else if (*p == 'l'){ size = 2; p++; if (*p == 'l'){ size = 3; p++; } }
        else if (*p == 'L'){ size = 2; p++; }
        else if (p[0] == 'I' && p[1] == '6' && p[2] == '4'){ size = 3; p += 3; }
        char conv = *p;
        if (!conv) break;
        if (conv != 'c' && conv != '[' && conv != 'n') while (is_space((unsigned char)in[i])) i++;
        if (conv == 'n'){ if (!skip){ uint32_t q = MR32(ap); ap += 4; if (size == 1) MR16(q) = (uint16_t)i; else MR32(q) = (uint32_t)i; } continue; }
        if (!in[i]) return conv_seen || got ? got : -1;  /* input ended before this conversion */
        conv_seen = 1;
        size_t max = width ? (size_t)width : (size_t)-1;
        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'o' || conv == 'x' || conv == 'X' || conv == 'p'){
            int base = conv == 'o' ? 8 : (conv == 'x' || conv == 'X' || conv == 'p') ? 16 : conv == 'i' ? 0 : 10;
            size_t j = i, n = 0; int neg = 0;
            if (n < max && (in[j] == '+' || in[j] == '-')){ neg = in[j] == '-'; j++; n++; }
            if (base == 0){
                if (n < max && in[j] == '0' && n + 1 < max && (in[j + 1] == 'x' || in[j + 1] == 'X')){ base = 16; j += 2; n += 2; }
                else if (n < max && in[j] == '0') base = 8;
                else base = 10;
            } else if (base == 16 && n + 1 < max && in[j] == '0' && (in[j + 1] == 'x' || in[j + 1] == 'X')){ j += 2; n += 2; }
            uint64_t v = 0; size_t nd = 0;
            for (; n < max && in[j]; j++, n++, nd++){
                int d = in[j] >= '0' && in[j] <= '9' ? in[j] - '0' : (in[j] | 0x20) >= 'a' && (in[j] | 0x20) <= 'z' ? (in[j] | 0x20) - 'a' + 10 : 99;
                if (d >= base) break;
                v = v * (uint64_t)base + (uint64_t)d;
            }
            if (!nd) break;                           /* no digits: a matching failure */
            if (neg) v = (uint64_t)0 - v;
            i = j;
            if (!skip){
                uint32_t q = MR32(ap); ap += 4;
                if (size == 1) MR16(q) = (uint16_t)v; else if (size == 3){ MR32(q) = (uint32_t)v; MR32(q + 4) = (uint32_t)(v >> 32); }
                else MR32(q) = (uint32_t)v;
                got++;
            }
        } else if (conv == 'e' || conv == 'E' || conv == 'f' || conv == 'g' || conv == 'G'){
            char t[512]; size_t len = float_text(in + i, max < 500 ? max : 500, t, sizeof t);
            if (!len) break;
            i += len;
            if (!skip){
                double d = strtod(t, 0);
                uint32_t q = MR32(ap); ap += 4;
                if (size == 2) put_d(q, d); else MRF32(q) = (float)d;
                got++;
            }
        } else if (conv == 'c'){
            size_t k = width ? (size_t)width : 1, n = 0;
            uint32_t q = skip ? 0 : MR32(ap);
            for (; n < k && in[i]; n++, i++) if (q) MR8(q + n) = (uint8_t)in[i];
            if (n < k && !n) break;
            if (!skip){ ap += 4; got++; }
        } else if (conv == 's' || conv == '['){
            uint8_t set[256]; int inv = 0;
            if (conv == '['){
                p++;
                if (*p == '^'){ inv = 1; p++; }
                memset(set, 0, sizeof set);
                const char *q0 = p;
                for (; *p && (*p != ']' || p == q0); p++){
                    if (p[1] == '-' && p[2] && p[2] != ']'){ for (int ch = (unsigned char)p[0]; ch <= (unsigned char)p[2]; ch++) set[ch] = 1; p += 2; }
                    else set[(unsigned char)*p] = 1;
                }
                if (!*p) p--;
            } else { for (int ch = 0; ch < 256; ch++) set[ch] = !is_space(ch); }
            size_t n = 0; uint32_t q = skip ? 0 : MR32(ap);
            for (; n < max && in[i] && (set[(unsigned char)in[i]] != 0) != inv; n++, i++) if (q) MR8(q + n) = (uint8_t)in[i];
            if (!n) break;
            if (q) MR8(q + n) = 0;
            if (!skip){ ap += 4; got++; }
        } else break;                                 /* a conversion msvcrt does not know: the end */
    }
    return got;
}

/* ---- qsort: in place, the comparator always given two elements of the array (as msvcrt does);
 * insertion for a few elements, heapsort above.  Not stable, as msvcrt's is not; D3DX sorts tables
 * whose keys do not tie, where every sort gives the same order ---- */
typedef struct { CPU *c; uint32_t base, width, cmp; uint8_t *tmp; } Sort;
static int s_cmp(Sort *s, uint32_t i, uint32_t j){
    uint32_t a[2] = { s->base + i * s->width, s->base + j * s->width };
    return (int32_t)crt_call(s->c, s->cmp, 2, a);
}
static void s_swap(Sort *s, uint32_t i, uint32_t j){
    uint8_t *a = g_mem + s->base + i * s->width, *b = g_mem + s->base + j * s->width;
    memcpy(s->tmp, a, s->width); memcpy(a, b, s->width); memcpy(b, s->tmp, s->width);
}
static void s_sift(Sort *s, uint32_t root, uint32_t n){
    for (;;){
        uint32_t big = root, l = 2 * root + 1, r = l + 1;
        if (l < n && s_cmp(s, l, big) > 0) big = l;
        if (r < n && s_cmp(s, r, big) > 0) big = r;
        if (big == root) return;
        s_swap(s, root, big); root = big;
    }
}
static void crt_qsort(CPU *c, uint32_t base, uint32_t num, uint32_t width, uint32_t cmp){
    if (num < 2 || !width || (uint64_t)num * width > 0x7FFFFFFFu) return;
    Sort s = { c, base, width, cmp, malloc(width) };
    if (!s.tmp) return;
    if (num <= 16){
        for (uint32_t i = 1; i < num; i++)
            for (uint32_t j = i; j > 0 && s_cmp(&s, j - 1, j) > 0; j--) s_swap(&s, j - 1, j);
    } else {
        for (uint32_t r = num / 2; r-- > 0; ) s_sift(&s, r, num);
        for (uint32_t n = num - 1; n > 0; n--){ s_swap(&s, 0, n); s_sift(&s, 0, n); }
    }
    free(s.tmp);
}

/* ---- _setjmp3: the buffer msvcrt fills (_JUMP_BUFFER: Ebp, Ebx, Edi, Esi, Esp, Eip, Registration,
 * TryLevel, Cookie 'VC20', UnwindFunc, UnwindData[6]); only longjmp would read it ---- */
static void crt_setjmp3(CPU *c){
    uint32_t b = A(c, 0), n = A(c, 1);
    MR32(b) = c->ebp; MR32(b + 4) = c->ebx; MR32(b + 8) = c->edi; MR32(b + 12) = c->esi;
    MR32(b + 16) = c->esp - 4u; MR32(b + 20) = c->eip;                /* esp at the call, the return address */
    uint32_t reg = c->fs_base ? MR32(c->fs_base) : 0xFFFFFFFFu;
    MR32(b + 24) = reg;
    MR32(b + 28) = reg != 0xFFFFFFFFu && mem_is_committed(reg + 12u) ? MR32(reg + 12u) : 0xFFFFFFFFu;
    MR32(b + 32) = 0x56433230u;
    MR32(b + 36) = n ? A(c, 2) : 0u;
    for (uint32_t k = 0; k < 6; k++) MR32(b + 40 + 4 * k) = k + 1 < n ? A(c, 3 + (int)k) : 0u;
}

/* the variable msvcrt exports and the lifted DLL reads through its IAT slot (module.h bind) */
uint32_t msvcrt_bind(const ImpEnt *e){
    if (strcasecmp(e->dll, "msvcrt.dll") || strcmp(e->name, "_adjust_fdiv")) return 0;
    static uint32_t var;
    if (!var){ var = gheap_alloc(1, 4, 1);           /* 0: no Pentium FDIV correction wanted */
        fprintf(stderr, "[msvcrt] _adjust_fdiv = 0 at 0x%08x\n", var); }
    return var;
}

int msvcrt_call(CPU *c, const char *n, uint32_t *out){
    int id = fn_id(n);
    if (id < 0) return 0;
    static uint8_t seen[F_COUNT];
    if (!seen[id]){ seen[id] = 1; fprintf(stderr, "[msvcrt] %s: first call (from 0x%08x)\n", n, c->eip); }
    uint32_t r = 0;
    switch (id){
    /* the process */
    case F_terminate: crt_stop(c, "terminate()", 3); break;
    case F_purecall:  crt_stop(c, "_purecall (a pure virtual function was called, R6025)", 3); break;
    case F_except_handler3: crt_stop(c, "_except_handler3 (exception dispatch)", 5); break;
    case F_cxxframe:  crt_stop(c, "__CxxFrameHandler (exception dispatch)", 5); break;
    case F_longjmp: {
        char w[96]; snprintf(w, sizeof w, "longjmp(0x%08x, %d) to the setjmp that returned to 0x%08x",
                             A(c, 0), (int)A(c, 1), A(c, 0) ? MR32(A(c, 0) + 20u) : 0u);
        crt_stop(c, w, 5); break; }
    case F_adjust_fdiv:                               /* a variable: bound by msvcrt_bind, never called */
        fprintf(stderr, "[msvcrt] _adjust_fdiv called as a function (from 0x%08x)\n", c->eip); break;
    case F_exit:
        g_exit_code = (int)A(c, 0);
        fprintf(stderr, "\n*** guest exit(%d) (msvcrt) - exiting\n", g_exit_code);
        if (g_exit_armed) longjmp(g_exit_jmp, 1);
        g_guest_exited = 1; break;
    case F_initterm:
        for (uint32_t p = A(c, 0), e = A(c, 1); p < e; p += 4u){ uint32_t f = MR32(p); if (f) crt_call(c, f, 0, 0); }
        break;
    case F_setjmp3: crt_setjmp3(c); r = 0; break;
    /* memory */
    case F_malloc: case F_opnew: r = crt_malloc(A(c, 0), 0); break;
    case F_calloc: { uint64_t t = (uint64_t)A(c, 0) * A(c, 1); r = t > HEAP_MAXREQ ? 0u : crt_malloc((uint32_t)t, 1); break; }
    case F_realloc: {
        uint32_t p = A(c, 0), sz = A(c, 1), h = crt_heap();
        if (!p) r = crt_malloc(sz, 0);
        else if (!sz){ crt_free(c, p); r = 0; }
        else r = h && sz <= HEAP_MAXREQ ? modheap_realloc(h, p, sz, 0) : 0u;
        break; }
    case F_free: case F_opdelete: crt_free(c, A(c, 0)); break;
    case F_strdup: {
        uint32_t s = A(c, 0);
        if (s){ uint32_t len = (uint32_t)strlen(S(s)) + 1u; r = crt_malloc(len, 0); if (r) memcpy(g_mem + r, g_mem + s, len); }
        break; }
    case F_memmove: memmove(g_mem + A(c, 0), g_mem + A(c, 1), A(c, 2)); r = A(c, 0); break;
    case F_qsort: crt_qsort(c, A(c, 0), A(c, 1), A(c, 2), A(c, 3)); break;
    /* strings and characters */
    case F_strchr: {
        uint8_t ch = (uint8_t)A(c, 1);
        for (uint32_t p = A(c, 0);; p++){ if (MR8(p) == ch){ r = p; break; } if (!MR8(p)) break; }
        break; }
    case F_stricmp: {
        uint32_t a = A(c, 0), b = A(c, 1); int d;
        for (;; a++, b++){
            int x = MR8(a), y = MR8(b);
            if (x >= 'A' && x <= 'Z') x += 32;
            if (y >= 'A' && y <= 'Z') y += 32;
            d = x - y;
            if (d || !x) break;
        }
        r = (uint32_t)d; break; }
    case F_wcslen: { uint32_t p = A(c, 0); while (MR16(p + 2u * r)) r++; break; }
    case F_wcstombs: {                                /* (dst, src, n): the C locale takes 0..255 only */
        uint32_t d = A(c, 0), s = A(c, 1), cnt = A(c, 2), k = 0;
        for (;; k++){
            uint32_t w = MR16(s + 2u * k);
            if (w > 255){ r = 0xFFFFFFFFu; break; }
            if (d){ if (k >= cnt){ r = k; break; } MR8(d + k) = (uint8_t)w; }
            if (!w){ r = k; break; }
        }
        break; }
    case F_toupper: { int32_t ch = (int32_t)A(c, 0); r = (uint32_t)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch); break; }
    case F_tolower: { int32_t ch = (int32_t)A(c, 0); r = (uint32_t)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch); break; }
    case F_isalpha: case F_isalnum: case F_isdigit: case F_isspace: case F_isxdigit: {
        uint32_t ch = A(c, 0), t = ch < 256 ? ctype_c(ch) : 0;
        uint32_t m = id == F_isalpha ? C1_ALPHA | C1_UPPER | C1_LOWER : id == F_isalnum ? C1_ALPHA | C1_UPPER | C1_LOWER | C1_DIGIT
                   : id == F_isdigit ? C1_DIGIT : id == F_isspace ? C1_SPACE : C1_XDIGIT;
        r = t & m; break; }
    case F_iswalpha: case F_iswdigit: case F_iswspace: case F_iswpunct: {
        uint32_t t = ctype_w(A(c, 0) & 0xFFFFu);
        uint32_t m = id == F_iswalpha ? C1_ALPHA | C1_UPPER | C1_LOWER : id == F_iswdigit ? C1_DIGIT : id == F_iswspace ? C1_SPACE : C1_PUNCT;
        r = t & m; break; }
    case F_setlocale: {                               /* (category, locale): "C", the one locale here */
        static uint32_t cstr;
        uint32_t cat = A(c, 0), loc = A(c, 1);
        if (!cstr){ cstr = gheap_alloc(1, 4, 1); MR8(cstr) = 'C'; }
        if (cat > 5u) r = 0;
        else if (!loc || !strcmp(S(loc), "C") || !S(loc)[0]) r = cstr;
        else {
            static int said = 0;
            if (said++ < 4) fprintf(stderr, "[msvcrt] setlocale(%u, \"%.40s\"): only \"C\" is here (from 0x%08x)\n", cat, S(loc), c->eip);
            r = 0;
        }
        break; }
    case F_tempnam: {
        static int said = 0;
        if (!said++) fprintf(stderr, "[msvcrt] _tempnam: no temporary files here, NULL (from 0x%08x)\n", c->eip);
        r = 0; break; }
    case F_strtime: { uint32_t b = A(c, 0); memcpy(g_mem + b, "12:00:00", 9); r = b; break; }   /* GetLocalTime's time (win32_ext.c) */
    case F_strdate: { uint32_t b = A(c, 0); memcpy(g_mem + b, "08/24/26", 9); r = b; break; }   /* and date, 2026-08-24 */
    case F_vsnprintf: r = (uint32_t)crt_vsnprintf(A(c, 0), A(c, 1), S(A(c, 2)), A(c, 3)); break;
    case F_sscanf: r = (uint32_t)crt_sscanf(S(A(c, 0)), S(A(c, 1)), c->esp + 8u); break;
    /* numbers */
    case F_atoi: case F_atol: r = crt_atol(S(A(c, 0))); break;
    case F_ultoa: {
        uint32_t v = A(c, 0), b = A(c, 1), radix = A(c, 2); char t[40]; int k = 0;
        if (radix < 2 || radix > 36) radix = 10;
        do { uint32_t d = v % radix; t[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= radix; } while (v);
        for (int i = 0; i < k; i++) MR8(b + (uint32_t)i) = (uint8_t)t[k - 1 - i];
        MR8(b + (uint32_t)k) = 0; r = b; break; }
    case F_srand: g_crt_rand = A(c, 0); break;
    case F_rand: g_crt_rand = g_crt_rand * 214013u + 2531011u; r = (g_crt_rand >> 16) & 0x7FFFu; break;
    case F_atof: {
        const char *s = S(A(c, 0)); while (is_space((unsigned char)*s)) s++;
        char t[512]; size_t len = float_text(s, 500, t, sizeof t);
        FPUSH(len ? strtod(t, 0) : 0.0); break; }
    case F_controlfp: {
        uint32_t mask = A(c, 1) & ~0x80000u & (0x0008001Fu | 0x40000u | 0x300u | 0x30000u);
        uint32_t cur = cw_abstract(c->fcw), nw = (cur & ~mask) | (A(c, 0) & mask);
        uint16_t was = c->fcw;
        c->fcw = cw_x87(nw, c->fcw);
        if ((c->fcw & 0x3Fu) != 0x3Fu && (was & 0x3Fu) == 0x3Fu){
            static int said = 0;
            if (said++ < 4) fprintf(stderr, "[msvcrt] _controlfp unmasks x87 exceptions (control word 0x%04x): the runtime raises"
                                            " none (from 0x%08x)\n", c->fcw, c->eip);
        }
        r = cw_abstract(c->fcw); break; }
    case F_fpclass: {
        double d = AD(c, 0); uint64_t b; memcpy(&b, &d, 8); int neg = signbit(d) != 0;
        if (isnan(d)) r = (b & 0x0008000000000000ull) ? 0x0002u : 0x0001u;
        else if (isinf(d)) r = neg ? 0x0004u : 0x0200u;
        else if (d == 0) r = neg ? 0x0020u : 0x0040u;
        else if (fpclassify(d) == FP_SUBNORMAL) r = neg ? 0x0010u : 0x0080u;
        else r = neg ? 0x0008u : 0x0100u;
        break; }
    case F_isnan: r = isnan(AD(c, 0)) ? 1u : 0u; break;
    case F_finite: r = isfinite(AD(c, 0)) ? 1u : 0u; break;
    case F_floor: FPUSH(floor(AD(c, 0))); break;
    case F_ceil: FPUSH(ceil(AD(c, 0))); break;
    case F_ldexp: FPUSH(ldexp(AD(c, 0), (int32_t)A(c, 2))); break;
    case F_frexp: { int e = 0; double m = frexp(AD(c, 0), &e); if (A(c, 2)) MR32(A(c, 2)) = (uint32_t)e; FPUSH(m); break; }
    case F_modf: { double ip = 0, f = modf(AD(c, 0), &ip); if (A(c, 2)) put_d(A(c, 2), ip); FPUSH(f); break; }
    /* the x87 helpers: the result at the caller's precision control, the x87's indefinite for a
     * domain error */
    case F_CIsqrt: case F_CIexp: case F_CIcosh: case F_CIsinh: case F_CItanh: case F_CIasin: case F_CIacos: {
        double x = ST(0), y;
        switch (id){
        case F_CIsqrt: y = sqrt(x); break;
        case F_CIexp: y = exp(x); break;
        case F_CIcosh: y = cosh(x); break;
        case F_CIsinh: y = sinh(x); break;
        case F_CItanh: y = tanh(x); break;
        case F_CIasin: y = asin(x); break;
        default: y = acos(x); break;
        }
        ST(0) = x87_pcround(c->fcw, x87_r1(x, y)); break; }
    case F_CIpow: case F_CIfmod: {
        double y = ST(0), x = ST(1), v = id == F_CIpow ? pow(x, y) : fmod(x, y);
        FPOP();
        ST(0) = x87_pcround(c->fcw, x87_r2(x, y, v)); break; }
    default: return 0;
    }
    *out = r;
    return 1;
}
