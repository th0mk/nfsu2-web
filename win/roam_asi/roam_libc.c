/* roam_libc.c -- the C library subset NFSU2FreeRoam.asi needs (it links no C
 * runtime: no DLL besides KERNEL32 and WS2_32, nothing that could clash with
 * the game's or the other mods' runtimes).  Built with -fno-builtin
 * -fno-tree-loop-distribute-patterns, so these loops are not turned back
 * into calls of themselves.
 *
 * vsnprintf: %d %i %u %x %X %o %c %s %p %f %%, flags - 0 + space #, width
 * and precision (also *), length hh h l ll z j t.  %f is exact for what the
 * log prints (up to 18 significant digits); no %e/%g (printed as %f).
 */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

void *memcpy(void *d, const void *s, size_t n){
    uint8_t *o = d; const uint8_t *i = s;
    while (n--) *o++ = *i++;
    return d;
}
void *memmove(void *d, const void *s, size_t n){
    uint8_t *o = d; const uint8_t *i = s;
    if (o == i || !n) return d;
    if (o < i){ while (n--) *o++ = *i++; }
    else { o += n; i += n; while (n--) *--o = *--i; }
    return d;
}
void *memset(void *d, int c, size_t n){
    uint8_t *o = d;
    while (n--) *o++ = (uint8_t)c;
    return d;
}
int memcmp(const void *a, const void *b, size_t n){
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
}
size_t strlen(const char *s){ size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b){
    while (*a && *a == *b){ a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}
int strncmp(const char *a, const char *b, size_t n){
    for (; n; n--, a++, b++){ if (*a != *b) return (uint8_t)*a - (uint8_t)*b; if (!*a) return 0; }
    return 0;
}
static int lower(int c){ return c >= 'A' && c <= 'Z' ? c + 32 : c; }
int strncasecmp(const char *a, const char *b, size_t n){
    for (; n; n--, a++, b++){
        int x = lower((uint8_t)*a), y = lower((uint8_t)*b);
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}
int strcasecmp(const char *a, const char *b){ return strncasecmp(a, b, (size_t)-1); }
char *strstr(const char *h, const char *n){                    /* runtime/names.h's name filter */
    if (!*n) return (char *)h;
    for (; *h; h++){
        const char *a = h, *b = n;
        while (*a && *a == *b){ a++; b++; }
        if (!*b) return (char *)h;
    }
    return 0;
}
char *strchr(const char *s, int c){                             /* runtime/names.h's name filter */
    for (;; s++){
        if (*s == (char)c) return (char *)s;
        if (!*s) return 0;
    }
}
long strtol(const char *s, char **end, int base){
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') p++;
    int neg = 0;
    if (*p == '+' || *p == '-'){ neg = *p == '-'; p++; }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')){ p += 2; base = 16; }
    else if (base == 0) base = p[0] == '0' ? 8 : 10;
    const char *start = p;
    unsigned long v = 0, lim = neg ? 0x80000000ul : 0x7FFFFFFFul;
    int over = 0;
    for (;; p++){
        int d = *p >= '0' && *p <= '9' ? *p - '0' : lower((uint8_t)*p) >= 'a' && lower((uint8_t)*p) <= 'z' ? lower((uint8_t)*p) - 'a' + 10 : 99;
        if (d >= base) break;
        if (v > (lim - (unsigned long)d) / (unsigned long)base) over = 1;
        else v = v * (unsigned long)base + (unsigned long)d;
    }
    if (end) *end = (char *)(p == start ? s : p);
    if (over) return neg ? (long)-2147483647 - 1 : 2147483647L;
    return neg ? (long)(0 - v) : (long)v;
}

/* ---- vsnprintf -------------------------------------------------------------------------- */
typedef struct { char *b; size_t n, k; } Out;
static void put(Out *o, char c){ if (o->k + 1 < o->n) o->b[o->k] = c; o->k++; }
static void pad(Out *o, char c, int w){ while (w-- > 0) put(o, c); }
static int utoa(char *t, uint64_t v, unsigned base, int upper){
    const char *dg = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do { t[n++] = dg[v % base]; v /= base; } while (v);
    return n;                                     /* reversed */
}
static void emit_num(Out *o, const char *sign, const char *rev, int nd, int prec, int width, int left, int zero){
    int sl = 0; while (sign[sl]) sl++;
    int digits = nd > prec ? nd : prec;
    int w = width - sl - digits;
    if (!left && !zero) pad(o, ' ', w);
    for (int i = 0; i < sl; i++) put(o, sign[i]);
    if (!left && zero) pad(o, '0', w);
    pad(o, '0', digits - nd);
    for (int i = nd - 1; i >= 0; i--) put(o, rev[i]);
    if (left) pad(o, ' ', w);
}
static void emit_f(Out *o, double v, int prec, int width, int left, int zero, int plus, int space){
    char t[64]; int n = 0;
    const char *sign = "";
    uint64_t bits; memcpy(&bits, &v, 8);
    if (bits >> 63){ sign = "-"; v = -v; } else if (plus) sign = "+"; else if (space) sign = " ";
    if (v != v){ emit_num(o, sign, "nan", 3, 0, width, left, 0); return; }
    if (v > 1.8e18){ emit_num(o, sign, "fni", 3, 0, width, left, 0); return; }   /* inf or too large for us */
    if (prec > 9) prec = 9;
    uint64_t scale = 1; for (int i = 0; i < prec; i++) scale *= 10u;
    double sv = v * (double)scale;
    uint64_t whole;
    if (sv < 1.8e19){
        uint64_t q = (uint64_t)sv; double fr = sv - (double)q;
        if (fr > 0.5 || (fr == 0.5 && (q & 1u))) q++;
        whole = q / scale; uint64_t frac = q % scale;
        for (int i = 0; i < prec; i++){ t[n++] = (char)('0' + frac % 10u); frac /= 10u; }
    } else {                                       /* a huge value: no fraction digits */
        whole = (uint64_t)v;
        for (int i = 0; i < prec; i++) t[n++] = '0';
    }
    if (prec) t[n++] = '.';
    n += utoa(t + n, whole, 10, 0);
    emit_num(o, sign, t, n, 0, width, left, zero);
}
int vsnprintf(char *buf, size_t size, const char *f, va_list ap){
    Out o = { buf, size, 0 };
    for (; *f; f++){
        if (*f != '%'){ put(&o, *f); continue; }
        f++;
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0, width = 0, prec = -1, len = 0;
        for (;; f++){
            if (*f == '-') left = 1; else if (*f == '0') zero = 1; else if (*f == '+') plus = 1;
            else if (*f == ' ') space = 1; else if (*f == '#') alt = 1; else break;
        }
        if (*f == '*'){ width = va_arg(ap, int); if (width < 0){ left = 1; width = -width; } f++; }
        else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.'){
            f++; prec = 0;
            if (*f == '*'){ prec = va_arg(ap, int); f++; } else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        for (;; f++){
            if (*f == 'h') len = len == -1 ? -2 : -1;
            else if (*f == 'l') len = len == 1 ? 2 : 1;
            else if (*f == 'z' || *f == 't') len = sizeof(size_t) == 8 ? 2 : 0;
            else if (*f == 'j') len = 2;
            else break;
        }
        char t[72]; int n;
        switch (*f){
        case 'd': case 'i': {
            int64_t v = len == 2 ? va_arg(ap, long long) : len == 1 ? va_arg(ap, long) : va_arg(ap, int);
            if (len == -1) v = (short)v; else if (len == -2) v = (signed char)v;
            uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
            n = utoa(t, u, 10, 0);
            emit_num(&o, v < 0 ? "-" : plus ? "+" : space ? " " : "", t, n, prec < 0 ? 1 : prec, width, left, zero && prec < 0);
            break; }
        case 'u': case 'x': case 'X': case 'o': {
            uint64_t v = len == 2 ? va_arg(ap, unsigned long long) : len == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            if (len == -1) v = (unsigned short)v; else if (len == -2) v = (unsigned char)v;
            unsigned base = *f == 'u' ? 10u : *f == 'o' ? 8u : 16u;
            n = utoa(t, v, base, *f == 'X');
            emit_num(&o, alt && base == 16 && v ? (*f == 'X' ? "0X" : "0x") : "", t, n, prec < 0 ? 1 : prec, width, left, zero && prec < 0);
            break; }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            n = utoa(t, v, 16, 0);
            emit_num(&o, "0x", t, n, 1, width, left, 0);
            break; }
        case 'c': { char c = (char)va_arg(ap, int); if (!left) pad(&o, ' ', width - 1); put(&o, c); if (left) pad(&o, ' ', width - 1); break; }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int sl = 0; while (s[sl] && (prec < 0 || sl < prec)) sl++;
            if (!left) pad(&o, ' ', width - sl);
            for (int i = 0; i < sl; i++) put(&o, s[i]);
            if (left) pad(&o, ' ', width - sl);
            break; }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            emit_f(&o, va_arg(ap, double), prec < 0 ? 6 : prec, width, left, zero, plus, space);
            break;
        case '%': put(&o, '%'); break;
        case 0: f--; break;
        default: put(&o, '%'); put(&o, *f); break;
        }
    }
    if (size) buf[o.k < size ? o.k : size - 1] = 0;
    return (int)o.k;
}
int snprintf(char *buf, size_t n, const char *fmt, ...){
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}
