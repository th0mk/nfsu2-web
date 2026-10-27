/* Win32 layer, third part: small imports answered as Windows answers them,
 * most of them new with Most Wanted (GAME.md 3.3).  win32_call (win32.c) asks
 * win32_misc_call after the other layers, under the guest lock; nothing here
 * blocks.
 *
 *   memory      GlobalMemoryStatusEx
 *   locale      GetDateFormatA, GetTimeFormatA (en-US, as Windows XP formats
 *               it: the runtime reports XP, GetVersion), CompareStringA/W
 *   process     SetStdHandle (GetStdHandle answers it), Set/GetEnvironment-
 *               VariableA; QueueUserAPC and CreateProcessA refused with a log
 *               line (FatalAppExitA ends the process: win32.c)
 *   keyboard    the US layout: GetKeyboardLayout, MapVirtualKeyA/ExA,
 *               ToUnicode, GetKeyState (keyboard.c's key state); SendInput
 *               accepted, nothing injected
 *   voice       winmm waveIn and waveOut and DSOUND #6 DirectSoundCapture-
 *               Create: no devices (no voice chat, PLAN D-23)
 *
 * Every refusal sets the last error Windows sets.  Logs are tagged [hle] and
 * [voice] and say each thing once. */
#include "cpu.h"
#include "kbd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void mem_commit(uint32_t addr, uint32_t size);
void win32_set_last_error(CPU *c, uint32_t e);
static uint32_t A(CPU *c, int i){ return MR32(c->esp + 4u*(uint32_t)i); }

#define ERROR_INVALID_HANDLE      6u
#define ERROR_NOT_SUPPORTED       50u
#define ERROR_INVALID_PARAMETER   87u
#define ERROR_INSUFFICIENT_BUFFER 122u
#define ERROR_ENVVAR_NOT_FOUND    203u
#define ERROR_INVALID_FLAGS       1004u

/* a guest range is readable/writable (every 16 KB page of it committed) */
static int g_ok(uint32_t p, uint32_t n){
    if (!p || !n) return p != 0;
    for (uint64_t a = p & ~0x3FFFu; a < (uint64_t)p + n; a += 0x4000u) if (a > 0xFFFFFFFFull || !mem_is_committed((uint32_t)a)) return 0;
    return 1;
}
/* a guest NUL-terminated string into out (cut at cap - 1); 0 if unreadable */
static int g_str(uint32_t p, char *out, size_t cap){
    size_t i = 0;
    if (!p) return 0;
    for (; i + 1 < cap; i++){
        if ((i == 0 || ((p + i) & 0x3FFFu) == 0) && !mem_is_committed(p + (uint32_t)i)) return 0;
        if (!(out[i] = (char)MR8(p + (uint32_t)i))) return 1;
    }
    out[i] = 0;
    return 1;
}

/* ---- GlobalMemoryStatusEx --------------------------------------------------
 * Its one caller in MW (0x6C1B28, settings defaults) drops the world and car
 * detail to 0 below 400 MiB of physical memory (Q-28).  The answer is the
 * machine GlobalMemoryStatus describes (win32_ext.c: 1 GiB, half free, load
 * 50 %), with every 64-bit field of the 64-byte MEMORYSTATUSEX filled. */
static void put64(uint32_t p, uint64_t v){ MR32(p) = (uint32_t)v; MR32(p + 4u) = (uint32_t)(v >> 32); }
static uint32_t memory_status_ex(CPU *c){
    uint32_t p = A(c,0);
    if (!g_ok(p, 64u) || MR32(p) != 64u){ win32_set_last_error(c, ERROR_INVALID_PARAMETER); return 0; }  /* dwLength */
    memset(g_mem + p + 4u, 0, 60u);
    MR32(p + 4u) = 50u;                       /* dwMemoryLoad */
    put64(p + 8u,  0x40000000ull);            /* ullTotalPhys: 1 GiB */
    put64(p + 16u, 0x20000000ull);            /* ullAvailPhys */
    put64(p + 24u, 0x80000000ull);            /* ullTotalPageFile (commit limit) */
    put64(p + 32u, 0x60000000ull);            /* ullAvailPageFile */
    put64(p + 40u, 0x7FFE0000ull);            /* ullTotalVirtual: a 32-bit process's 2 GB */
    put64(p + 48u, 0x60000000ull);            /* ullAvailVirtual */
    /* ullAvailExtendedVirtual (56): always 0 */
    { static int sh = 0; if (!sh++) fprintf(stderr, "[hle] GlobalMemoryStatusEx: 1024 MiB of physical memory, 512 MiB free (from 0x%08x)\n", c->eip); }
    return 1;
}

/* ---- en-US date and time formats -------------------------------------------
 * The pictures of GetDateFormatA (d dd ddd dddd, M MM MMM MMMM, y yy yyyy,
 * g gg) and GetTimeFormatA (h hh H HH, m mm, s ss, t tt), text in single
 * quotes as it is ('' is a quote), every other character as it is.  The
 * locale is always en-US (GetLocaleInfoA answers 0409), its formats those of
 * Windows XP: short date M/d/yyyy, long date dddd, MMMM dd, yyyy, year and
 * month MMMM, yyyy, time h:mm:ss tt. */
typedef struct { int y, mo, d, h, mi, s, ms; } Stime;
static const char *const k_day[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const k_mon[12] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                       "September", "October", "November", "December" };
static int leap_year(int y){ return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int month_days(int y, int m){ static const uint8_t d[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 }; return d[m-1] + (m == 2 && leap_year(y)); }
/* day of the week of a Gregorian date, 0 = Sunday (Windows works it out and ignores wDayOfWeek) */
static int week_day(int y, int m, int d){
    static const uint8_t t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m-1] + d) % 7;
}
static void read_stime(uint32_t p, Stime *t){
    t->y = MR16(p); t->mo = MR16(p + 2u); t->d = MR16(p + 6u); t->h = MR16(p + 8u);
    t->mi = MR16(p + 10u); t->s = MR16(p + 12u); t->ms = MR16(p + 14u);
}
/* the runtime's local time when the caller gives none: GetLocalTime's fixed date (win32_ext.c) */
static void local_stime(Stime *t){ *t = (Stime){ 2026, 8, 24, 12, 0, 0, 0 }; }
static int date_ok(const Stime *t){ return t->y >= 1601 && t->y <= 30827 && t->mo >= 1 && t->mo <= 12 && t->d >= 1 && t->d <= month_days(t->y, t->mo); }
static int time_ok(const Stime *t){ return t->h <= 23 && t->mi <= 59 && t->s <= 59 && t->ms <= 999; }

typedef struct { char *b; size_t n, cap; int over; } Out;
static void out_s(Out *o, const char *s){
    for (; *s; s++){ if (o->n + 1 < o->cap) o->b[o->n++] = *s; else o->over = 1; }
    o->b[o->n] = 0;
}
static void out_num(Out *o, int v, int width){ char t[16]; snprintf(t, sizeof t, "%0*d", width, v); out_s(o, t); }
/* a quoted run starting at f[0] == '\'': copy it out, return the characters used
 * ('' outside a run is a quote too) */
static size_t out_quoted(Out *o, const char *f){
    size_t i = 1;
    if (f[1] == '\''){ out_s(o, "'"); return 2; }
    for (; f[i]; i++){
        if (f[i] == '\''){ if (f[i + 1] == '\''){ out_s(o, "'"); i++; continue; } return i + 1; }
        char ch[2] = { f[i], 0 }; out_s(o, ch);
    }
    return i;                                 /* unterminated: to the end, as Windows takes it */
}
static void format_date(Out *o, const char *f, const Stime *t){
    while (*f){
        char ch = *f; size_t k = 1;
        if (ch == '\''){ f += out_quoted(o, f); continue; }
        while (f[k] == ch) k++;
        if (ch == 'd'){
            if (k == 1) out_num(o, t->d, 1); else if (k == 2) out_num(o, t->d, 2);
            else { char ab[4]; const char *nm = k_day[week_day(t->y, t->mo, t->d)];
                   if (k == 3){ memcpy(ab, nm, 3); ab[3] = 0; nm = ab; } out_s(o, nm); }
        } else if (ch == 'M'){
            if (k == 1) out_num(o, t->mo, 1); else if (k == 2) out_num(o, t->mo, 2);
            else { char ab[4]; const char *nm = k_mon[t->mo - 1];
                   if (k == 3){ memcpy(ab, nm, 3); ab[3] = 0; nm = ab; } out_s(o, nm); }
        } else if (ch == 'y'){
            if (k >= 3) out_num(o, t->y, 1); else out_num(o, t->y % 100, (int)k);
        } else if (ch == 'g') out_s(o, "A.D.");                /* the Gregorian era of en-US */
        else { char s[2] = { ch, 0 }; for (size_t i = 0; i < k; i++) out_s(o, s); }
        f += k;
    }
}
#define TIME_NOMINUTESORSECONDS 0x1u
#define TIME_NOSECONDS          0x2u
#define TIME_NOTIMEMARKER       0x4u
#define TIME_FORCE24HOURFORMAT  0x8u
/* An element the flags leave out takes the text since the element before it
 * along (the separator in front of it), as Windows does. */
static void format_time(Out *o, const char *f, const Stime *t, uint32_t fl){
    size_t last = 0;                          /* the end of the last element written */
    while (*f){
        char ch = *f; size_t k = 1;
        if (ch == '\''){ f += out_quoted(o, f); continue; }
        while (f[k] == ch) k++;
        int skip = 0;
        if (ch == 'h' || ch == 'H'){
            int h = (ch == 'H' || (fl & TIME_FORCE24HOURFORMAT)) ? t->h : (t->h % 12 ? t->h % 12 : 12);
            out_num(o, h, k >= 2 ? 2 : 1);
        } else if (ch == 'm'){
            if (fl & TIME_NOMINUTESORSECONDS) skip = 1; else out_num(o, t->mi, k >= 2 ? 2 : 1);
        } else if (ch == 's'){
            if (fl & (TIME_NOMINUTESORSECONDS | TIME_NOSECONDS)) skip = 1; else out_num(o, t->s, k >= 2 ? 2 : 1);
        } else if (ch == 't'){
            if (fl & TIME_NOTIMEMARKER) skip = 1; else out_s(o, k >= 2 ? (t->h < 12 ? "AM" : "PM") : (t->h < 12 ? "A" : "P"));
        } else { char s[2] = { ch, 0 }; for (size_t i = 0; i < k; i++) out_s(o, s); f += k; continue; }
        if (skip){ o->n = last; o->b[o->n] = 0; } else last = o->n;
        f += k;
    }
}
/* the text into the caller's buffer: Windows' count rules (cch 0 asks for the size, NUL included) */
static uint32_t put_text(CPU *c, const char *s, uint32_t dst, int cch){
    uint32_t len = (uint32_t)strlen(s) + 1u;
    if (cch == 0) return len;
    if (cch < 0 || !g_ok(dst, (uint32_t)cch)){ win32_set_last_error(c, ERROR_INVALID_PARAMETER); return 0; }
    if ((uint32_t)cch < len){ win32_set_last_error(c, ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(g_mem + dst, s, len);
    return len;
}
#define DATE_SHORTDATE        0x01u
#define DATE_LONGDATE         0x02u
#define DATE_USE_ALT_CALENDAR 0x04u
#define DATE_YEARMONTH        0x08u
#define DATE_LTRREADING       0x10u
#define DATE_RTLREADING       0x20u
#define LOCALE_USE_CP_ACP     0x40000000u
#define LOCALE_NOUSEROVERRIDE 0x80000000u
/* GetDateFormatA(LCID, dwFlags, const SYSTEMTIME *lpDate, LPCSTR lpFormat, LPSTR lpDateStr, int cchDate) */
static uint32_t get_date_format(CPU *c){
    uint32_t fl = A(c,1), pt = A(c,2), pf = A(c,3), dst = A(c,4); int cch = (int)A(c,5);
    uint32_t kinds = fl & (DATE_SHORTDATE | DATE_LONGDATE | DATE_YEARMONTH);
    if ((fl & ~(DATE_SHORTDATE | DATE_LONGDATE | DATE_USE_ALT_CALENDAR | DATE_YEARMONTH | DATE_LTRREADING | DATE_RTLREADING |
                LOCALE_USE_CP_ACP | LOCALE_NOUSEROVERRIDE)) || (kinds & (kinds - 1u)) ||
        (fl & (DATE_LTRREADING | DATE_RTLREADING)) == (DATE_LTRREADING | DATE_RTLREADING) ||
        (pf && (kinds || (fl & LOCALE_NOUSEROVERRIDE)))){
        win32_set_last_error(c, ERROR_INVALID_FLAGS); return 0; }
    Stime t; char fmt[256];
    if (pt){ if (!g_ok(pt, 16u)){ win32_set_last_error(c, ERROR_INVALID_PARAMETER); return 0; } read_stime(pt, &t); }
    else local_stime(&t);
    if (!date_ok(&t)){ win32_set_last_error(c, ERROR_INVALID_PARAMETER); return 0; }
    if (pf){ if (!g_str(pf, fmt, sizeof fmt)){ win32_set_last_error(c, ERROR_INVALID_PARAMETER); return 0; } }
    else snprintf(fmt, sizeof fmt, "%s", kinds == DATE_LONGDATE ? "dddd, MMMM dd, yyyy" : kinds == DATE_YEARMONTH ? "MMMM, yyyy" : "M/d/yyyy");
    char buf[512]; Out o = { buf, 0, sizeof buf, 0 }; buf[0] = 0;
    format_date(&o, fmt, &t);
    return put_text(c, buf, dst, cch);
}
/* GetTimeFormatA(LCID, dwFlags, const SYSTEMTIME *lpTime, LPCSTR lpFormat, LPSTR lpTimeStr, int cchTime) */
static uint32_t get_time_format(CPU *c){
    uint32_t fl = A(c,1), pt = A(c,2), pf = A(c,3), dst = A(c,4); int cch = (int)A(c,5);
    if ((fl & ~(TIME_NOMINUTESORSECONDS | TIME_NOSECONDS | TIME_NOTIMEMARKER | TIME_FORCE24HOURFORMAT |
                LOCALE_USE_CP_ACP | LOCALE_NOUSEROVERRIDE)) || (pf && (fl & LOCALE_NOUSEROVERRIDE))){
        win32_set_last_error(c, ERROR_INVALID_FLAGS); return 0; }
    Stime t; char fmt[256];
    if (pt){ if (!g_ok(pt, 16u)){ win32_set_last_error(c, ERROR_INVALID_PARAMETER); return 0; } read_stime(pt, &t); }
    else local_stime(&t);
    if (!time_ok(&t)){ win32_set_last_error(c, ERROR_INVALID_PARAMETER); return 0; }
    if (pf){ if (!g_str(pf, fmt, sizeof fmt)){ win32_set_last_error(c, ERROR_INVALID_PARAMETER); return 0; } }
    else snprintf(fmt, sizeof fmt, "h:mm:ss tt");
    char buf[512]; Out o = { buf, 0, sizeof buf, 0 }; buf[0] = 0;
    format_time(&o, fmt, &t, fl);
    return put_text(c, buf, dst, cch);
}

/* ---- CompareStringA/W ------------------------------------------------------
 * Windows' word sort, as its documentation describes it: symbols before
 * digits before letters; letters alphabetical whatever their case, then
 * accents (a letter without one first), then case (lower case first); the
 * hyphen and the apostrophe count only when everything else is equal, so
 * "coop" and "co-op" sort together ("coop" first), unless SORT_STRINGSORT
 * makes them symbols; control characters and NUL count for nothing.
 * NORM_IGNORECASE, NORM_IGNORENONSPACE (accents) and NORM_IGNORESYMBOLS drop
 * those levels.  Letters are Latin-1's (A's bytes are code page 1252, the
 * runtime's ACP); other characters sort after them by code.  Callers in the
 * two games are their C runtimes' locale paths (strcoll and friends), which
 * the "C" locale never takes. */
#define NORM_IGNORECASE     0x00000001u
#define NORM_IGNORENONSPACE 0x00000002u
#define NORM_IGNORESYMBOLS  0x00000004u
#define SORT_STRINGSORT     0x00001000u
#define CMP_FLAGS_OK (NORM_IGNORECASE | NORM_IGNORENONSPACE | NORM_IGNORESYMBOLS | SORT_STRINGSORT | 0x8u /* SORT_DIGITSASNUMBERS */ | \
                      0x10u /* LINGUISTIC_IGNORECASE */ | 0x20u /* LINGUISTIC_IGNOREDIACRITIC */ | 0x10000u /* NORM_IGNOREKANATYPE */ | \
                      0x20000u /* NORM_IGNOREWIDTH */ | 0x08000000u /* NORM_LINGUISTIC_CASING */ | LOCALE_USE_CP_ACP)
/* code page 1252's 0x80..0x9F */
static const uint16_t k_cp1252[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178 };
/* Latin-1 letters 0xC0..0xFF: base letter (lower case), accent (0 none), upper case; 0 = not a letter */
static const char k_l1_base[65] = "aaaaaaaceeeeiiiidnooooo\0ouuuuyts" "aaaaaaaceeeeiiiidnooooo\0ouuuuyty";
static const uint8_t k_l1_acc[64] = { 1,2,3,4,5,6,0,7, 1,2,3,5, 1,2,3,5, 8,4, 1,2,3,4,5,0, 9,1,2,3,5, 2,0,0,
                                      1,2,3,4,5,6,0,7, 1,2,3,5, 1,2,3,5, 8,4, 1,2,3,4,5,0, 9,1,2,3,5, 2,0,5 };
/* one character's weights; returns 0 for a character that counts for nothing.
 * kind: 1 symbol, 2 digit, 3 letter, 4 other; special: the hyphen or the apostrophe in a word sort */
typedef struct { uint32_t prim; uint8_t acc, upper, special; } Wt;
static int weigh(uint32_t u, uint32_t fl, Wt w[2]){
    memset(w, 0, 2 * sizeof *w);
    if (u < 0x20u || u == 0x7Fu || (u >= 0x80u && u < 0xA0u)) return 0;   /* control characters, NUL */
    if ((u == '-' || u == '\'') && !(fl & SORT_STRINGSORT)){ w[0].special = 1; w[0].prim = u; return 1; }
    if (u >= '0' && u <= '9'){ w[0].prim = (2u << 24) | (u - '0'); return 1; }
    if ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')){
        w[0].prim = (3u << 24) | (uint32_t)((u | 0x20u) - 'a'); w[0].upper = u <= 'Z'; return 1; }
    if (u == 0xC6u || u == 0xE6u || u == 0x152u || u == 0x153u || u == 0xDFu){   /* AE, OE, sharp s: two letters */
        char a = u == 0xDFu ? 's' : (u == 0xC6u || u == 0xE6u) ? 'a' : 'o', b = u == 0xDFu ? 's' : 'e';
        uint8_t up = u == 0xC6u || u == 0x152u;
        w[0].prim = (3u << 24) | (uint32_t)(a - 'a'); w[0].upper = up;
        w[1].prim = (3u << 24) | (uint32_t)(b - 'a'); w[1].upper = up;
        return 2; }
    if (u >= 0xC0u && u <= 0xFFu && k_l1_base[u - 0xC0u]){
        char b = k_l1_base[u - 0xC0u];
        w[0].prim = (3u << 24) | (uint32_t)(b == 't' && (u == 0xDEu || u == 0xFEu) ? 26 : b - 'a');   /* thorn after z */
        w[0].acc = k_l1_acc[u - 0xC0u]; w[0].upper = u < 0xE0u; return 1; }
    if (u == 0x160u || u == 0x161u || u == 0x17Du || u == 0x17Eu || u == 0x178u){   /* S, Z with caron, Y diaeresis */
        w[0].prim = (3u << 24) | (uint32_t)(u == 0x178u ? 'y' - 'a' : (u < 0x17Du ? 's' : 'z') - 'a');
        w[0].acc = u == 0x178u ? 5 : 10; w[0].upper = u == 0x160u || u == 0x17Du || u == 0x178u; return 1; }
    if (u < 0x80u || (u >= 0xA0u && u < 0xC0u) || u == 0xD7u || u == 0xF7u || (u >= 0x2000u && u < 0x2200u)){
        w[0].prim = (1u << 24) | u; return 1; }            /* punctuation, space, signs */
    w[0].prim = (4u << 24) | u; return 1;
}
#define CMP_MAX 2048
/* the weights of a string; n = its characters (wide: UTF-16 units) */
static int weigh_all(uint32_t p, int n, int wide, uint32_t fl, Wt *out, int cap){
    int k = 0;
    for (int i = 0; i < n && k + 2 <= cap; i++){
        uint32_t u = wide ? MR16(p + 2u * (uint32_t)i) : MR8(p + (uint32_t)i);
        if (!wide && u >= 0x80u && u < 0xA0u) u = k_cp1252[u - 0x80u];
        Wt w[2]; int m = weigh(u, fl, w);
        for (int j = 0; j < m; j++){
            if ((fl & NORM_IGNORESYMBOLS) && (w[j].special || (w[j].prim >> 24) == 1u)) continue;
            if (w[j].special) w[j].prim = ((uint32_t)i << 8) | w[j].prim;        /* where it stands, and which */
            out[k++] = w[j];
        }
    }
    return k;
}
static int cmp3(uint64_t a, uint64_t b){ return a < b ? -1 : a > b; }
static uint32_t compare_strings(CPU *c, int wide){
    uint32_t fl = A(c,1), p1 = A(c,2), p2 = A(c,4); int n1 = (int)A(c,3), n2 = (int)A(c,5);
    if (fl & ~CMP_FLAGS_OK){ win32_set_last_error(c, ERROR_INVALID_FLAGS); return 0; }
    if (!p1 || !p2 || !g_ok(p1, 1u) || !g_ok(p2, 1u)){ win32_set_last_error(c, ERROR_INVALID_PARAMETER); return 0; }
    uint32_t cs = wide ? 2u : 1u;
    if (n1 < 0){ n1 = 0; while (n1 < CMP_MAX * 4 && (wide ? MR16(p1 + 2u * (uint32_t)n1) : MR8(p1 + (uint32_t)n1))) n1++; }
    if (n2 < 0){ n2 = 0; while (n2 < CMP_MAX * 4 && (wide ? MR16(p2 + 2u * (uint32_t)n2) : MR8(p2 + (uint32_t)n2))) n2++; }
    if (!g_ok(p1, n1 ? (uint32_t)n1 * cs : 1u) || !g_ok(p2, n2 ? (uint32_t)n2 * cs : 1u)){
        win32_set_last_error(c, ERROR_INVALID_PARAMETER); return 0; }
    static Wt a[CMP_MAX], b[CMP_MAX];               /* guest lock held: one caller at a time */
    int na = weigh_all(p1, n1, wide, fl, a, CMP_MAX), nb = weigh_all(p2, n2, wide, fl, b, CMP_MAX);
    int r = 0;
    /* level 1: the characters (special ones aside); level 2: accents; level 3: case; then the special ones */
    for (int lvl = 1; lvl <= 4 && !r; lvl++){
        if ((lvl == 2 && (fl & (NORM_IGNORENONSPACE | 0x20u))) || (lvl == 3 && (fl & (NORM_IGNORECASE | 0x10u)))) continue;
        int i = 0, j = 0;
        for (;;){
            while (i < na && (a[i].special != 0) != (lvl == 4)) i++;
            while (j < nb && (b[j].special != 0) != (lvl == 4)) j++;
            if (i >= na || j >= nb){ r = (i < na) - (j < nb); break; }
            uint64_t x = lvl == 2 ? a[i].acc : lvl == 3 ? a[i].upper : a[i].prim;
            uint64_t y = lvl == 2 ? b[j].acc : lvl == 3 ? b[j].upper : b[j].prim;
            if ((r = cmp3(x, y))) break;
            i++; j++;
        }
    }
    return r < 0 ? 1u : r > 0 ? 3u : 2u;              /* CSTR_LESS_THAN, CSTR_EQUAL, CSTR_GREATER_THAN */
}

/* ---- the std handles and the environment ----------------------------------- */
/* GetStdHandle (win32.c): what SetStdHandle set, else the runtime's own
 * pseudo handles 0x10..0x13 as always */
static uint32_t g_std[3]; static int g_std_set[3];
uint32_t win32_std_handle(uint32_t which){
    uint32_t i = 0xFFFFFFF6u - which;                 /* STD_INPUT_HANDLE -10, OUTPUT -11, ERROR -12 */
    if (i < 3u && g_std_set[i]) return g_std[i];
    return 0x10u + (which & 3u);
}
/* The process environment the guest sets (SetEnvironmentVariableA); the C
 * runtimes read their own copy, made from GetEnvironmentStrings at start
 * (win32.c: empty), and only _putenv writes here. */
#define ENV_MAX 64
static struct { char *name, *value; } g_env[ENV_MAX];
static int env_find(const char *nm){
    for (int i = 0; i < ENV_MAX; i++) if (g_env[i].name && !strcasecmp(g_env[i].name, nm)) return i;   /* names: any case */
    return -1;
}

/* ---- the keyboard: the US layout ------------------------------------------
 * Virtual-key codes and set-1 scan codes (0xE0xx: extended keys, 0xE11D
 * Pause), as the US keyboard layout (0x04090409) maps them.  The runtime's
 * key state is DirectInput's (keyboard.c): a scan code, 0x80 added for an
 * extended key. */
static const uint16_t k_vk_sc[256] = {
    [0x08]=0x0E, [0x09]=0x0F, [0x0C]=0x4C, [0x0D]=0x1C, [0x10]=0x2A, [0x11]=0x1D, [0x12]=0x38, [0x13]=0xE11D,
    [0x14]=0x3A, [0x1B]=0x01, [0x20]=0x39, [0x21]=0xE049, [0x22]=0xE051, [0x23]=0xE04F, [0x24]=0xE047,
    [0x25]=0xE04B, [0x26]=0xE048, [0x27]=0xE04D, [0x28]=0xE050, [0x2C]=0x54, [0x2D]=0xE052, [0x2E]=0xE053,
    ['0']=0x0B, ['1']=0x02, ['2']=0x03, ['3']=0x04, ['4']=0x05, ['5']=0x06, ['6']=0x07, ['7']=0x08, ['8']=0x09, ['9']=0x0A,
    ['A']=0x1E, ['B']=0x30, ['C']=0x2E, ['D']=0x20, ['E']=0x12, ['F']=0x21, ['G']=0x22, ['H']=0x23, ['I']=0x17,
    ['J']=0x24, ['K']=0x25, ['L']=0x26, ['M']=0x32, ['N']=0x31, ['O']=0x18, ['P']=0x19, ['Q']=0x10, ['R']=0x13,
    ['S']=0x1F, ['T']=0x14, ['U']=0x16, ['V']=0x2F, ['W']=0x11, ['X']=0x2D, ['Y']=0x15, ['Z']=0x2C,
    [0x5B]=0xE05B, [0x5C]=0xE05C, [0x5D]=0xE05D,
    [0x60]=0x52, [0x61]=0x4F, [0x62]=0x50, [0x63]=0x51, [0x64]=0x4B, [0x65]=0x4C, [0x66]=0x4D, [0x67]=0x47,
    [0x68]=0x48, [0x69]=0x49, [0x6A]=0x37, [0x6B]=0x4E, [0x6D]=0x4A, [0x6E]=0x53, [0x6F]=0xE035,
    [0x70]=0x3B, [0x71]=0x3C, [0x72]=0x3D, [0x73]=0x3E, [0x74]=0x3F, [0x75]=0x40, [0x76]=0x41, [0x77]=0x42,
    [0x78]=0x43, [0x79]=0x44, [0x7A]=0x57, [0x7B]=0x58, [0x7C]=0x64, [0x7D]=0x65, [0x7E]=0x66, [0x7F]=0x67,
    [0x80]=0x68, [0x81]=0x69, [0x82]=0x6A, [0x83]=0x6B, [0x84]=0x6C, [0x85]=0x6D, [0x86]=0x6E, [0x87]=0x76,
    [0x90]=0xE045, [0x91]=0x46, [0xA0]=0x2A, [0xA1]=0x36, [0xA2]=0x1D, [0xA3]=0xE01D, [0xA4]=0x38, [0xA5]=0xE038,
    [0xBA]=0x27, [0xBB]=0x0D, [0xBC]=0x33, [0xBD]=0x0C, [0xBE]=0x34, [0xBF]=0x35, [0xC0]=0x29,
    [0xDB]=0x1A, [0xDC]=0x2B, [0xDD]=0x1B, [0xDE]=0x28, [0xE2]=0x56,
};
/* scan code -> virtual key, telling left from right (MAPVK_VSC_TO_VK_EX) */
static uint32_t sc_to_vk_ex(uint32_t sc){
    uint32_t pre = sc >> 8, s = sc & 0xFFu;
    if (pre == 0xE1u) return s == 0x1Du ? 0x13u : 0u;                    /* Pause */
    if (pre && pre != 0xE0u) return 0;
    if (s == 0 || s >= 0x80u) return 0;                                 /* a break code: no translation */
    if (pre == 0xE0u){
        switch (s){
        case 0x1C: return 0x0D; case 0x1D: return 0xA3; case 0x35: return 0x6F; case 0x37: return 0x2C;
        case 0x38: return 0xA5; case 0x45: return 0x90; case 0x47: return 0x24; case 0x48: return 0x26;
        case 0x49: return 0x21; case 0x4B: return 0x25; case 0x4D: return 0x27; case 0x4F: return 0x23;
        case 0x50: return 0x28; case 0x51: return 0x22; case 0x52: return 0x2D; case 0x53: return 0x2E;
        case 0x5B: return 0x5B; case 0x5C: return 0x5C; case 0x5D: return 0x5D;
        default:   return 0;
        }
    }
    switch (s){                                                         /* the numeric keypad's own: navigation keys */
    case 0x2A: return 0xA0; case 0x36: return 0xA1; case 0x1D: return 0xA2; case 0x38: return 0xA4;
    case 0x45: return 0x90; case 0x47: return 0x24; case 0x48: return 0x26; case 0x49: return 0x21;
    case 0x4B: return 0x25; case 0x4C: return 0x0C; case 0x4D: return 0x27; case 0x4F: return 0x23;
    case 0x50: return 0x28; case 0x51: return 0x22; case 0x52: return 0x2D; case 0x53: return 0x2E;
    case 0x54: return 0x2C;
    }
    for (uint32_t vk = 1; vk < 256u; vk++) if (k_vk_sc[vk] == s) return vk;   /* the rest have one key each */
    return 0;
}
static uint32_t map_virtual_key(uint32_t code, uint32_t type){
    switch (type){
    case 0:                                                                       /* MAPVK_VK_TO_VSC: no prefix */
        if (code >= 256u) return 0;
        return k_vk_sc[code] == 0xE11Du ? 0x45u : (k_vk_sc[code] & 0xFFu);        /* Pause: its 0x45 */
    case 4: return code < 256u ? k_vk_sc[code] : 0u;                              /* MAPVK_VK_TO_VSC_EX */
    case 1: {                                                                     /* MAPVK_VSC_TO_VK: no left or right */
        uint32_t vk = sc_to_vk_ex(code);
        return vk == 0xA0u || vk == 0xA1u ? 0x10u : vk == 0xA2u || vk == 0xA3u ? 0x11u : vk == 0xA4u || vk == 0xA5u ? 0x12u : vk; }
    case 3: return sc_to_vk_ex(code);                                             /* MAPVK_VSC_TO_VK_EX */
    case 2:                                                                       /* MAPVK_VK_TO_CHAR: unshifted */
        if (code >= 'A' && code <= 'Z') return code;
        if (code >= '0' && code <= '9') return code;
        if (code >= 0x60u && code <= 0x69u) return '0' + (code - 0x60u);
        switch (code){
        case 0x08: return 0x08; case 0x09: return 0x09; case 0x0D: return 0x0D; case 0x1B: return 0x1B; case 0x20: return ' ';
        case 0x6A: return '*'; case 0x6B: return '+'; case 0x6D: return '-'; case 0x6E: return '.'; case 0x6F: return '/';
        case 0xBA: return ';'; case 0xBB: return '='; case 0xBC: return ','; case 0xBD: return '-'; case 0xBE: return '.';
        case 0xBF: return '/'; case 0xC0: return '`'; case 0xDB: return '['; case 0xDC: return '\\'; case 0xDD: return ']';
        case 0xDE: return '\''; case 0xE2: return '\\';
        }
        return 0;
    }
    return 0;
}
/* the character a key types on the US layout, 0 for none */
static uint32_t us_char(uint32_t vk, int shift, int caps, int ctrl, int alt){
    static const char shifted_digit[10] = { ')', '!', '@', '#', '$', '%', '^', '&', '*', '(' };
    static const struct { uint8_t vk; char lo, hi, ctl; } oem[] = {
        { 0xBA, ';', ':', 0 }, { 0xBB, '=', '+', 0 }, { 0xBC, ',', '<', 0 }, { 0xBD, '-', '_', 0 }, { 0xBE, '.', '>', 0 },
        { 0xBF, '/', '?', 0 }, { 0xC0, '`', '~', 0 }, { 0xDB, '[', '{', 0x1B }, { 0xDC, '\\', '|', 0x1C }, { 0xDD, ']', '}', 0x1D },
        { 0xDE, '\'', '"', 0 }, { 0xE2, '\\', '|', 0x1C } };
    if (ctrl && alt) return 0;                                    /* AltGr: the US layout has nothing there */
    if (vk >= 'A' && vk <= 'Z') return ctrl ? vk - 'A' + 1u : (shift ^ caps) ? vk : vk + 32u;
    if (vk >= '0' && vk <= '9') return ctrl ? 0u : shift ? (uint32_t)shifted_digit[vk - '0'] : vk;
    if (vk >= 0x60u && vk <= 0x69u) return '0' + (vk - 0x60u);    /* number pad, Num Lock on */
    for (size_t i = 0; i < sizeof oem / sizeof *oem; i++)
        if (oem[i].vk == vk) return ctrl ? (uint32_t)(uint8_t)oem[i].ctl : (uint32_t)(uint8_t)(shift ? oem[i].hi : oem[i].lo);
    switch (vk){
    case 0x20: return ' ';
    case 0x0D: return ctrl ? '\n' : '\r';
    case 0x08: return ctrl ? 0x7Fu : 0x08u;
    case 0x09: return ctrl ? 0u : '\t';
    case 0x1B: return 0x1B;
    case 0x6A: return '*'; case 0x6B: return '+'; case 0x6D: return '-'; case 0x6E: return '.'; case 0x6F: return '/';
    }
    return 0;
}
/* GetKeyState: down from keyboard.c; toggled flips at each press this call sees */
static uint8_t g_key_prev[256], g_key_tog[256];
static int vk_down(uint32_t vk){
    switch (vk){                                                  /* either side */
    case 0x10: return kbd_key_down(0x2A) || kbd_key_down(0x36);
    case 0x11: return kbd_key_down(0x1D) || kbd_key_down(0x9D);
    case 0x12: return kbd_key_down(0x38) || kbd_key_down(0xB8);
    }
    uint32_t sc = vk < 256u ? k_vk_sc[vk] : 0u;
    if (!sc) return 0;
    if (sc == 0xE11Du) return kbd_key_down(0xC5);                 /* DIK_PAUSE */
    return kbd_key_down((sc & 0xFFu) | (sc >> 8 == 0xE0u ? 0x80u : 0u));
}
static uint32_t get_key_state(uint32_t vk){
    if (vk >= 256u) return 0;
    int d = vk_down(vk);
    if (d && !g_key_prev[vk]) g_key_tog[vk] ^= 1u;
    g_key_prev[vk] = (uint8_t)d;
    return (uint32_t)(int32_t)(int16_t)((d ? 0xFF80u : 0u) | g_key_tog[vk]);   /* a SHORT, sign-extended */
}

/* ---- the voice devices: none ------------------------------------------------ */
#define MMSYSERR_BADDEVICEID 2u
#define MMSYSERR_INVALHANDLE 5u
#define MMSYSERR_NODRIVER    6u
#define WAVE_MAPPER          0xFFFFFFFFu
#define DSERR_NODRIVER       0x88780078u
static const char *const k_wave[] = {
    "waveOutGetNumDevs", "waveInGetNumDevs", "waveOutGetDevCapsA", "waveInGetDevCapsA", "waveOutOpen", "waveInOpen",
    "waveOutClose", "waveOutPrepareHeader", "waveOutUnprepareHeader", "waveOutWrite", "waveOutReset", "waveOutGetPosition",
    "waveOutSetVolume", "waveInClose", "waveInPrepareHeader", "waveInUnprepareHeader", "waveInAddBuffer", "waveInStart",
    "waveInStop", "waveInReset", "waveInGetPosition" };
static void voice_log(CPU *c, int k, const char *name, uint32_t r){
    static uint32_t seen = 0;
    if (seen & (1u << k)) return;
    seen |= 1u << k;
    fprintf(stderr, "[voice] %s -> %s: no voice devices (no voice chat) (from 0x%08x)\n", name,
            r == DSERR_NODRIVER ? "DSERR_NODRIVER" : r == MMSYSERR_BADDEVICEID ? "MMSYSERR_BADDEVICEID" :
            r == MMSYSERR_INVALHANDLE ? "MMSYSERR_INVALHANDLE" : r == MMSYSERR_NODRIVER ? "MMSYSERR_NODRIVER" : "0 devices", c->eip);
}
/* 1 and *r when n is one of the voice imports */
static int voice_call(CPU *c, const char *dll, const char *n, uint32_t *r){
    if (!strcmp(n, "#6") && dll && !strcasecmp(dll, "DSOUND.dll")){
        /* DirectSoundCaptureCreate(lpcGUID, LPDIRECTSOUNDCAPTURE *ppDSC, pUnkOuter) */
        uint32_t pp = A(c,1);
        if (g_ok(pp, 4u)) MR32(pp) = 0;
        *r = DSERR_NODRIVER; voice_log(c, 31, "DirectSoundCaptureCreate", *r);
        return 1;
    }
    if (strncmp(n, "wave", 4)) return 0;
    int k = -1;
    for (int i = 0; i < (int)(sizeof k_wave / sizeof *k_wave); i++) if (!strcmp(n, k_wave[i])){ k = i; break; }
    if (k < 0) return 0;
    if (k < 2) *r = 0;                                            /* GetNumDevs */
    else if (k < 4) *r = A(c,0) == WAVE_MAPPER ? MMSYSERR_NODRIVER : MMSYSERR_BADDEVICEID;   /* GetDevCaps(id, ...) */
    else if (k < 6){                                              /* Open(phw, id, ...) */
        uint32_t ph = A(c,0);
        if (g_ok(ph, 4u)) MR32(ph) = 0;
        *r = A(c,1) == WAVE_MAPPER ? MMSYSERR_NODRIVER : MMSYSERR_BADDEVICEID;
    }
    else if (k == 12) *r = MMSYSERR_NODRIVER;                     /* waveOutSetVolume: a handle or a device id */
    else *r = MMSYSERR_INVALHANDLE;                               /* no handle was ever opened */
    voice_log(c, k, n, *r);
    return 1;
}

int win32_misc_call(CPU *c, const char *dll, const char *n, uint32_t *out){
    uint32_t r = 0;
    if (voice_call(c, dll, n, &r)) { /* winmm wave*, DSOUND #6 */ }
    else if (!strcmp(n, "GlobalMemoryStatusEx")) r = memory_status_ex(c);
    else if (!strcmp(n, "GetDateFormatA")) r = get_date_format(c);
    else if (!strcmp(n, "GetTimeFormatA")) r = get_time_format(c);
    else if (!strcmp(n, "CompareStringA")) r = compare_strings(c, 0);
    else if (!strcmp(n, "CompareStringW")) r = compare_strings(c, 1);
    else if (!strcmp(n, "SetStdHandle")){                         /* (nStdHandle, hHandle) */
        uint32_t i = 0xFFFFFFF6u - A(c,0);
        if (i < 3u){ g_std[i] = A(c,1); g_std_set[i] = 1; r = 1; }
        else { win32_set_last_error(c, ERROR_INVALID_HANDLE); r = 0; } }
    else if (!strcmp(n, "SetEnvironmentVariableA")){              /* (lpName, lpValue): NULL value deletes */
        char nm[256], val[4096]; uint32_t pv = A(c,1);
        if (!g_str(A(c,0), nm, sizeof nm) || !nm[0] || strchr(nm + 1, '=') || (pv && !g_str(pv, val, sizeof val))){
            win32_set_last_error(c, ERROR_INVALID_PARAMETER); r = 0; }
        else {
            int i = env_find(nm);
            if (!pv){ if (i >= 0){ free(g_env[i].name); free(g_env[i].value); g_env[i].name = g_env[i].value = 0; } r = 1; }
            else {
                if (i < 0) for (int j = 0; j < ENV_MAX && i < 0; j++) if (!g_env[j].name) i = j;
                if (i < 0){ win32_set_last_error(c, 8u); r = 0; }            /* ERROR_NOT_ENOUGH_MEMORY */
                else {
                    char *v = strdup(val);
                    if (!g_env[i].name) g_env[i].name = strdup(nm);
                    free(g_env[i].value); g_env[i].value = v; r = 1;
                }
            }
        } }
    else if (!strcmp(n, "GetEnvironmentVariableA")){              /* (lpName, lpBuffer, nSize): length without the NUL, or the size needed */
        char nm[256]; int i = g_str(A(c,0), nm, sizeof nm) ? env_find(nm) : -1;
        if (i < 0){ win32_set_last_error(c, ERROR_ENVVAR_NOT_FOUND); r = 0; }
        else {
            uint32_t len = (uint32_t)strlen(g_env[i].value), dst = A(c,1), cap = A(c,2);
            if (cap <= len || !g_ok(dst, cap)) r = len + 1u;
            else { memcpy(g_mem + dst, g_env[i].value, len + 1u); r = len; }
        } }
    /* --- keyboard --- */
    else if (!strcmp(n, "GetKeyboardLayout")) r = 0x04090409u;    /* (idThread): US English */
    else if (!strcmp(n, "MapVirtualKeyA")) r = map_virtual_key(A(c,0), A(c,1));
    else if (!strcmp(n, "MapVirtualKeyExA")) r = map_virtual_key(A(c,0), A(c,1));   /* the layout handle: always US */
    else if (!strcmp(n, "GetKeyState")) r = get_key_state(A(c,0));
    else if (!strcmp(n, "ToUnicode")){
        /* (wVirtKey, wScanCode, const BYTE lpKeyState[256], LPWSTR pwszBuff, int cchBuff, wFlags): characters written */
        uint32_t vk = A(c,0) & 0xFFu, sc = A(c,1), ks = A(c,2), buf = A(c,3); int cch = (int)A(c,4);
        r = 0;
        if (!(sc & 0x8000u) && g_ok(ks, 256u)){                   /* a key-up types nothing */
            int shift = (MR8(ks + 0x10u) | MR8(ks + 0xA0u) | MR8(ks + 0xA1u)) & 0x80;
            int ctrl  = (MR8(ks + 0x11u) | MR8(ks + 0xA2u) | MR8(ks + 0xA3u)) & 0x80;
            int alt   = (MR8(ks + 0x12u) | MR8(ks + 0xA4u) | MR8(ks + 0xA5u)) & 0x80;
            uint32_t ch = us_char(vk, shift != 0, MR8(ks + 0x14u) & 1, ctrl != 0, alt != 0);
            if (ch && cch >= 1 && g_ok(buf, (uint32_t)cch * 2u)){
                MR16(buf) = (uint16_t)ch;
                if (cch >= 2) MR16(buf + 2u) = 0;
                r = 1;
            }
        } }
    else if (!strcmp(n, "SendInput")){                            /* (cInputs, pInputs, cbSize): accepted, not injected */
        uint32_t cnt = A(c,0);
        if (A(c,2) != 28u || !g_ok(A(c,1), cnt * 28u)){ win32_set_last_error(c, ERROR_INVALID_PARAMETER); r = 0; }
        else {
            r = cnt;
            static int sh = 0; if (!sh++)
                fprintf(stderr, "[hle] SendInput(%u input(s), first of type %u) accepted, nothing injected (from 0x%08x)\n",
                        cnt, cnt ? MR32(A(c,1)) : 0u, c->eip);
        } }
    /* --- refused, with a log line --- */
    else if (!strcmp(n, "QueueUserAPC")){                         /* (pfnAPC, hThread, dwData) */
        static int sh = 0; if (sh++ < 8)
            fprintf(stderr, "[hle] QueueUserAPC(routine 0x%08x, thread 0x%08x) refused: no APCs (from 0x%08x)\n", A(c,0), A(c,1), c->eip);
        win32_set_last_error(c, ERROR_NOT_SUPPORTED); r = 0; }
    else if (!strcmp(n, "CreateProcessA")){                       /* (lpApplicationName, lpCommandLine, ... 10) */
        char app[128] = "", cmd[192] = "";
        if (!g_str(A(c,0), app, sizeof app)) app[0] = 0;
        if (!g_str(A(c,1), cmd, sizeof cmd)) cmd[0] = 0;
        static int sh = 0; if (sh++ < 8)
            fprintf(stderr, "[hle] CreateProcessA(\"%s\", \"%s\") refused: no other processes (from 0x%08x)\n", app, cmd, c->eip);
        win32_set_last_error(c, ERROR_NOT_SUPPORTED); r = 0; }
    else return 0;
    *out = r;
    return 1;
}
