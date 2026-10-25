/* heap_stress.c -- runtime/heap.c on its own: concurrency, realloc and zero
 * semantics, VirtualAlloc edge cases, fragmentation, and NFSU2_HEAPCHECK
 * against planted bugs (heap track, adversarial review).
 *
 *   clang -O1 -Iruntime tests/heap_stress.c runtime/heap.c runtime/mem.c -o out/heap_stress
 *   out/heap_stress                 all checks; exit 0 when they pass
 *
 * The planted-bug cases run in child processes of the same binary with
 * NFSU2_HEAPCHECK=1 (out/heap_stress hc <case>): double free, invalid and
 * interior frees, write after free (caught when the block leaves the
 * quarantine and in the exit sweep), a stale pointer after a moving
 * realloc, a one-byte overrun, and a use after free of a large block, which
 * must fault. */
#include "cpu.h"
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

extern char **environ;
void mem_init(void); void mem_commit(uint32_t, uint32_t);
void heap_init(void); void heap_report(void);
int heap_api_kind(const char *); uint32_t heap_api(CPU *, int);
uint32_t gheap_alloc_c(CPU *, int, uint32_t, int); void gheap_free_c(CPU *, int, uint32_t);
void heap_totals(uint64_t *, uint64_t *, uint64_t *, uint64_t *);
uint64_t heap_internal_live(int);
extern int g_gh_com, g_gh_gbuf;

/* heap.c's links into the rest of the runtime */
#include "game.h"
const GameOps g_game = { .id = "ug2", .crt_lo = 0x0075b000u, .crt_hi = 0x00772000u };   /* UG2's C runtime */
void guest_backtrace(CPU *c, int n){ (void)c; (void)n; }
long autotest_frame(void){ return 0; }
void win32_set_last_error(CPU *c, uint32_t e){ (void)c; (void)e; }
void com_mem_stats(uint64_t *a, uint64_t *b, uint64_t *o){ *a = *b = *o = 0; }
void metal_mem_stats(uint64_t *a, unsigned *l, unsigned *t, uint64_t *b){ *a = 0; *l = *t = 0; *b = 0; }
int win32_obj_live(void){ return 0; }
int fileio_open_handles(void){ return 0; }
int win32_threads_live(void){ return 0; }

static int fails = 0;
#define CHK(name, cond) do{ if (cond) printf("  ok    %s\n", name); else { printf("  FAIL  %s\n", name); fails++; } }while(0)

/* A Win32 heap call as the import bridge makes it: stdcall arguments at esp. */
static uint32_t call(CPU *c, uint32_t stk, const char *fn, int n, const uint32_t *a){
    c->esp = stk;
    for (int i = n - 1; i >= 0; i--){ c->esp -= 4; MR32(c->esp) = a[i]; }
    c->eip = 0x00401000u;
    return heap_api(c, heap_api_kind(fn));
}
#define H(c, s, fn, ...) ({ uint32_t _a[] = { __VA_ARGS__ }; call(c, s, fn, (int)(sizeof _a / 4), _a); })
static uint32_t rnd32(uint32_t *s){ *s = *s * 1103515245u + 12345u; return *s >> 7; }

/* ---- 1. host threads allocating at the same time ------------------------- */
enum { NT = 8, NB = 400 };
static volatile int g_stop = 0;
static uint32_t g_h2;                                /* a HeapCreate heap */
typedef struct { int id; uint32_t p[NB], n[NB]; int kind[NB]; long ops, bad, zbad, nullr; } TS;
static TS g_ts[NT];
static void fill(uint32_t p, uint32_t n, int id, int k){ for (uint32_t i = 0; i < n; i += 7) MR8(p + i) = (uint8_t)(id * 31 + k + i); }
static int intact(uint32_t p, uint32_t n, int id, int k){
    for (uint32_t i = 0; i < n; i += 7) if (MR8(p + i) != (uint8_t)(id * 31 + k + i)) return 0;
    return 1;
}
static int zeroed(uint32_t p, uint32_t from, uint32_t to){
    for (uint32_t k = from; k < to; k += 13) if (MR8(p + k)) return 0;
    return 1;
}
static void *worker(void *arg){
    TS *t = arg; CPU cpu; memset(&cpu, 0, sizeof cpu);
    uint32_t stk = 0x00E00000u + 0x10000u * (uint32_t)t->id + 0xF000u, s = 777u + (uint32_t)t->id * 99991u;
    while (!g_stop){
        int i = (int)(rnd32(&s) % NB);
        if (t->p[i]){
            if (!intact(t->p[i], t->n[i], t->id, i)) t->bad++;
            if (t->kind[i] == 0 && rnd32(&s) % 4 == 0){           /* HeapReAlloc, half with ZERO */
                uint32_t ns = 1 + rnd32(&s) % ((rnd32(&s) & 7) ? 3000u : 1500000u);
                int zero = rnd32(&s) & 1;
                uint32_t np = H(&cpu, stk, "HeapReAlloc", g_h2, zero ? 8u : 0u, t->p[i], ns);
                if (!np){ t->nullr++; continue; }
                if (!intact(np, t->n[i] < ns ? t->n[i] : ns, t->id, i)) t->bad++;
                if (zero && ns > t->n[i] && !zeroed(np, t->n[i], ns)) t->zbad++;
                t->p[i] = np; t->n[i] = ns; fill(np, ns, t->id, i);
            } else {
                if (t->kind[i] == 0) H(&cpu, stk, "HeapFree", g_h2, 0u, t->p[i]);
                else gheap_free_c(&cpu, t->kind[i] == 1 ? g_gh_com : g_gh_gbuf, t->p[i]);
                t->p[i] = 0;
            }
        } else {                                                  /* guest heap, COM objects, Lock buffers */
            int kind = (int)(rnd32(&s) % 3), zero = rnd32(&s) & 1;
            uint32_t n = 1 + rnd32(&s) % ((rnd32(&s) & 15) ? 2000u : 1200000u), p;
            if (kind == 0) p = H(&cpu, stk, "HeapAlloc", g_h2, zero ? 8u : 0u, n);
            else p = gheap_alloc_c(&cpu, kind == 1 ? g_gh_com : g_gh_gbuf, n, zero);
            if (!p){ t->nullr++; continue; }
            if (p & 15u) t->bad++;
            if (zero && !zeroed(p, 0, n)) t->zbad++;
            t->p[i] = p; t->n[i] = n; t->kind[i] = kind; fill(p, n, t->id, i);
        }
        t->ops++;
    }
    return 0;
}
static int cmpu(const void *a, const void *b){ const uint32_t *x = a, *y = b; return x[0] < y[0] ? -1 : x[0] > y[0]; }
static void threads(void){
    printf("1. %d host threads, alloc/free/realloc on a guest heap and both internal heaps, 3 s\n", NT);
    CPU c0; memset(&c0, 0, sizeof c0);
    g_h2 = H(&c0, 0x00EF0000u, "HeapCreate", 0u, 0x1000u, 0u);
    pthread_t th[NT];
    for (int k = 0; k < NT; k++){ g_ts[k].id = k; pthread_create(&th[k], 0, worker, &g_ts[k]); }
    sleep(3); g_stop = 1;
    for (int k = 0; k < NT; k++) pthread_join(th[k], 0);
    long ops = 0, bad = 0, zbad = 0, nullr = 0; int na = 0, ov = 0;
    static uint32_t all[NT * NB][2];
    for (int k = 0; k < NT; k++){
        ops += g_ts[k].ops; bad += g_ts[k].bad; zbad += g_ts[k].zbad; nullr += g_ts[k].nullr;
        for (int i = 0; i < NB; i++) if (g_ts[k].p[i]){
            all[na][0] = g_ts[k].p[i]; all[na][1] = g_ts[k].n[i]; na++;
            if (!intact(g_ts[k].p[i], g_ts[k].n[i], k, i)) bad++; } }
    qsort(all, (size_t)na, sizeof all[0], cmpu);
    for (int i = 1; i < na; i++) if (all[i-1][0] + all[i-1][1] > all[i][0]) ov++;
    printf("  %ld operations, %d live blocks at the end\n", ops, na);
    CHK("contents intact", bad == 0);
    CHK("zeroed allocations and realloc tails read zero", zbad == 0);
    CHK("no failed allocation", nullr == 0);
    CHK("no overlapping live blocks", ov == 0);
    for (int k = 0; k < NT; k++) for (int i = 0; i < NB; i++) if (g_ts[k].p[i]){
        if (g_ts[k].kind[i] == 0) H(&c0, 0x00EF0000u, "HeapFree", g_h2, 0u, g_ts[k].p[i]);
        else gheap_free_c(&c0, g_ts[k].kind[i] == 1 ? g_gh_com : g_gh_gbuf, g_ts[k].p[i]); }
    uint64_t live; heap_totals(&live, 0, 0, 0);
    CHK("all freed: guest heaps 0 live bytes", live == 0);
    CHK("all freed: internal heaps 0 live bytes", heap_internal_live(g_gh_com) == 0 && heap_internal_live(g_gh_gbuf) == 0);
}

/* ---- 2. HeapReAlloc / GlobalReAlloc ---------------------------------------- */
static void realloc_zero(void){
    printf("2. HeapReAlloc shrink and grow, HEAP_ZERO_MEMORY, HEAP_REALLOC_IN_PLACE_ONLY, GlobalReAlloc\n");
    CPU c; memset(&c, 0, sizeof c); uint32_t s = 0x00EE0000u, h = g_h2;
    uint32_t a = H(&c, s, "HeapAlloc", h, 0u, 1000u);
    memset(g_mem + a, 0xAB, 1000);
    uint32_t guard = H(&c, s, "HeapAlloc", h, 0u, 64u);
    uint32_t b = H(&c, s, "HeapReAlloc", h, 0u, a, 100u);
    CHK("shrink stays in place", b == a);
    CHK("shrink: HeapSize 100", H(&c, s, "HeapSize", h, 0u, b) == 100u);
    uint32_t q = H(&c, s, "HeapAlloc", h, 0u, 200u);
    CHK("the shrunk tail is reused", q > a && q < a + 1000u);
    H(&c, s, "HeapFree", h, 0u, q);
    uint32_t g2 = H(&c, s, "HeapReAlloc", h, 8u, b, 1000u);
    int keep = 1; for (uint32_t i = 0; i < 100; i++) keep &= MR8(g2 + i) == 0xAB;
    CHK("grow in place into the freed tail", g2 == a);
    CHK("grow + ZERO: old bytes kept, new bytes zero (no stale 0xAB)", keep && zeroed(g2, 100, 1000));
    memset(g_mem + g2, 0xCD, 1000);
    uint32_t g3 = H(&c, s, "HeapReAlloc", h, 8u, g2, 50000u);
    keep = 1; for (uint32_t i = 0; i < 1000; i++) keep &= MR8(g3 + i) == 0xCD;
    CHK("moving grow + ZERO: data kept, tail zero", keep && zeroed(g3, 1000, 50000));
    uint32_t x = H(&c, s, "HeapAlloc", h, 0u, 256u), y = H(&c, s, "HeapAlloc", h, 0u, 256u);
    CHK("IN_PLACE_ONLY grow that cannot fit returns NULL", H(&c, s, "HeapReAlloc", h, 0x10u, x, 100000u) == 0);
    CHK("  ... and keeps the block", H(&c, s, "HeapSize", h, 0u, x) == 256u);
    uint32_t M = H(&c, s, "HeapAlloc", h, 0u, 3u << 20);
    memset(g_mem + M, 0x55, 3u << 20);
    uint32_t M2 = H(&c, s, "HeapReAlloc", h, 0u, M, 1u << 20);
    uint32_t M3 = H(&c, s, "HeapReAlloc", h, 8u, M2, 3u << 20);
    CHK("large block: shrink and regrow in place, ZERO tail zero", M2 == M && M3 == M && zeroed(M3, 1u << 20, 3u << 20));
    uint32_t G = H(&c, s, "GlobalAlloc", 0u, 100u); H(&c, s, "GlobalAlloc", 0u, 100u);
    CHK("GlobalReAlloc of a fixed block without GMEM_MOVEABLE cannot move: NULL", H(&c, s, "GlobalReAlloc", G, 100000u, 0u) == 0);
    uint32_t G3 = H(&c, s, "GlobalReAlloc", G, 100000u, 2u | 0x40u);
    CHK("GlobalReAlloc GMEM_MOVEABLE|GMEM_ZEROINIT moves, tail zero", G3 && zeroed(G3, 100, 100000));
    H(&c, s, "HeapFree", h, 0u, x); H(&c, s, "HeapFree", h, 0u, y); H(&c, s, "HeapFree", h, 0u, guard);
    H(&c, s, "HeapFree", h, 0u, g3); H(&c, s, "HeapFree", h, 0u, M3);
}

/* ---- 3. VirtualAlloc ---------------------------------------------------------- */
static void virtual(void){
    printf("3. VirtualAlloc / VirtualFree / VirtualQuery edge cases\n");
    CPU c; memset(&c, 0, sizeof c); uint32_t s = 0x00ED0000u, mbi = 0x00EC8000u;
    uint32_t a = H(&c, s, "VirtualAlloc", 0u, 0x5000u, 0x1000u, 4u);
    H(&c, s, "VirtualQuery", a, mbi, 28u);
    CHK("MEM_COMMIT alone at NULL reserves and commits", a && !(a & 0xFFFFu) && MR32(mbi + 16) == 0x1000u);
    uint32_t b = H(&c, s, "VirtualAlloc", 0x40008000u, 0x10000u, 0x3000u, 4u);
    H(&c, s, "VirtualQuery", 0x4000F000u + 0x8000u, mbi, 28u);
    CHK("RESERVE|COMMIT at an unaligned fixed address: 64 KB base, committed up to addr+size",
        b == 0x40000000u && MR32(mbi + 16) == 0x1000u);
    CHK("a fixed reserve over a reserved range fails", H(&c, s, "VirtualAlloc", 0x40008000u, 0x1000u, 0x2000u, 4u) == 0);
    uint32_t r = H(&c, s, "VirtualAlloc", 0u, 0x10000u, 0x2000u, 4u);
    CHK("commit past the region end fails", H(&c, s, "VirtualAlloc", r + 0xF000u, 0x2000u, 0x1000u, 4u) == 0);
    H(&c, s, "VirtualAlloc", r, 0x10000u, 0x1000u, 4u);
    memset(g_mem + r, 0x99, 0x10000);
    H(&c, s, "VirtualFree", r + 0x2000u, 0x1000u, 0x4000u);
    H(&c, s, "VirtualAlloc", r + 0x2000u, 0x1000u, 0x1000u, 4u);
    CHK("decommit/recommit of a 4 KB page inside a 16 KB host page reads zero", MR32(r + 0x2000u) == 0 && MR32(r + 0x2FFCu) == 0);
    CHK("  ... its neighbours keep their data", MR32(r + 0x1000u) == 0x99999999u && MR32(r + 0x3000u) == 0x99999999u);
    CHK("decommit with size 0 at the base", H(&c, s, "VirtualFree", r, 0u, 0x4000u) == 1u);
    H(&c, s, "VirtualAlloc", r + 0x1000u, 0x1000u, 0x1000u, 4u);
    CHK("  ... recommit reads zero", MR32(r + 0x1000u) == 0 && MR32(r + 0x1FFCu) == 0);
    H(&c, s, "VirtualFree", r, 0u, 0x8000u);
    uint32_t r2 = H(&c, s, "VirtualAlloc", 0u, 0x10000u, 0x3000u, 4u);
    CHK("a released range reserved again reads zero", MR32(r2) == 0 && MR32(r2 + 0x1004u) == 0);
    uint32_t hb = H(&c, s, "HeapAlloc", g_h2, 0u, 100u);
    CHK("VirtualFree(MEM_RELEASE) of a heap segment is refused", H(&c, s, "VirtualFree", hb & ~0xFFFFu, 0u, 0x8000u) == 0);
    CHK("  ... the heap block stays valid", H(&c, s, "HeapSize", g_h2, 0u, hb) == 100u);
    H(&c, s, "HeapFree", g_h2, 0u, hb);
    CHK("size 0 and 2 GB fail cleanly", !H(&c, s, "VirtualAlloc", 0u, 0u, 0x3000u, 4u) && !H(&c, s, "VirtualAlloc", 0u, 0x80000000u, 0x2000u, 4u));
    CHK("HeapAlloc of 0x7FFF0000 and 0xFFFFFFF0 fails cleanly",
        !H(&c, s, "HeapAlloc", g_h2, 0u, 0x7FFF0000u) && !H(&c, s, "HeapAlloc", g_h2, 0u, 0xFFFFFFF0u));
}

/* ---- 4. fragmentation ----------------------------------------------------------- */
static void fragmentation(void){
    printf("4. 200 'race loads': 6,000 mixed blocks and a 61 MB pool each, freed in random order, 5 kept\n");
    CPU c; memset(&c, 0, sizeof c); uint32_t s = 0x00EB0000u, seed = 4242;
    static uint32_t keep[1000], tmp[6000]; int nk = 0;
    uint64_t va0, va, vmax = 0; heap_totals(0, 0, &va0, 0);
    for (int load = 0; load < 200; load++){
        for (int i = 0; i < 6000; i++) tmp[i] = H(&c, s, "HeapAlloc", g_h2, 0u, 16 + rnd32(&seed) % ((rnd32(&seed) & 31) ? 4000u : 900000u));
        uint32_t big = H(&c, s, "HeapAlloc", g_h2, 0u, 61440172u);
        for (int i = 0; i < 6000; i++){ int j = (int)(rnd32(&seed) % 6000u); uint32_t t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t; }
        for (int i = 0; i < 6000; i++){
            if (i < 5 && nk < 1000) keep[nk++] = tmp[i];
            else H(&c, s, "HeapFree", g_h2, 0u, tmp[i]); }
        H(&c, s, "HeapFree", g_h2, 0u, big);
        if (load >= 100 && nk){ H(&c, s, "HeapFree", g_h2, 0u, keep[0]); memmove(keep, keep + 1, (size_t)--nk * 4); }
        heap_totals(0, 0, &va, 0); if (va > vmax) vmax = va;
    }
    printf("  VA %llu MB before, at most %llu MB, %llu MB after (%d long-lived blocks)\n", (unsigned long long)(va0 >> 20),
           (unsigned long long)(vmax >> 20), (unsigned long long)(va >> 20), nk);
    CHK("address space stays bounded (< 400 MB of 1,280)", vmax < (400ull << 20));
    for (int i = 0; i < nk; i++) H(&c, s, "HeapFree", g_h2, 0u, keep[i]);
}

/* ---- 5. NFSU2_HEAPCHECK against planted bugs (child processes) ------------------- */
static void planted(const char *what){
    CPU c; memset(&c, 0, sizeof c); uint32_t s = 0x00EA0000u;
    uint32_t h = H(&c, s, "HeapCreate", 0u, 0x1000u, 0u);
    uint32_t p = H(&c, s, "HeapAlloc", h, 0u, !strcmp(what, "overrun") ? 61u : !strcmp(what, "largeuaf") ? (2u << 20) : 64u);
    if (!strcmp(what, "double")){ H(&c, s, "HeapFree", h, 0u, p); H(&c, s, "HeapFree", h, 0u, p); }
    else if (!strcmp(what, "invalid")){ H(&c, s, "HeapFree", h, 0u, 0x00E00100u); H(&c, s, "HeapFree", h, 0u, p + 16); }
    else if (!strcmp(what, "uaf")){                       /* caught when it leaves the (1 MB) quarantine */
        H(&c, s, "HeapFree", h, 0u, p); MR32(p + 8) = 0x12345678u;
        for (int i = 0; i < 4000; i++){ uint32_t q = H(&c, s, "HeapAlloc", h, 0u, 4096u); H(&c, s, "HeapFree", h, 0u, q); } }
    else if (!strcmp(what, "uafexit")){ H(&c, s, "HeapFree", h, 0u, p); MR32(p + 8) = 0x12345678u; }
    else if (!strcmp(what, "realloc")){ H(&c, s, "HeapReAlloc", h, 0u, p, 5000u); MR32(p) = 0xBADBADu; }
    else if (!strcmp(what, "overrun")){ MR8(p + 61) = 0; H(&c, s, "HeapFree", h, 0u, p); }
    else if (!strcmp(what, "largeuaf")){ H(&c, s, "HeapFree", h, 0u, p); fflush(stdout); MR32(p + 100) = 1;
                                         printf("NO FAULT\n"); }
    heap_report();
}
static void heapcheck(const char *self){
    printf("5. NFSU2_HEAPCHECK=1 against planted bugs\n");
    static const struct { const char *what, *want; } k[] = {
        { "double",   "DOUBLE FREE" },        { "invalid",  "INVALID FREE (bad header)" },
        { "uaf",      "WRITE AFTER FREE: " },  { "uafexit",  "WRITE AFTER FREE (exit sweep)" },
        { "realloc",  "WRITE AFTER FREE (exit sweep)" }, { "overrun", "HEAP OVERRUN (tail canary)" },
        { "largeuaf", 0 } };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++){
        char cmd[512]; snprintf(cmd, sizeof cmd, "NFSU2_HEAPCHECK=1 NFSU2_HEAPCHECK_QUAR=1 '%s' hc %s 2>&1", self, k[i].what);
        static char buf[1 << 16]; size_t n = 0;
        FILE *f = popen(cmd, "r");
        if (f){ size_t m; while (n < sizeof buf - 1 && (m = fread(buf + n, 1, sizeof buf - 1 - n, f)) > 0) n += m; }
        buf[n] = 0;
        int st = f ? pclose(f) : -1;
        char name[96]; snprintf(name, sizeof name, "%s detected", k[i].what);
        if (k[i].want) CHK(name, st == 0 && strstr(buf, k[i].want));
        else CHK(name, (WIFSIGNALED(st) || (WIFEXITED(st) && WEXITSTATUS(st) > 128)) && !strstr(buf, "NO FAULT"));
    }
}

int main(int argc, char **argv){
    mem_init();
    mem_commit(0x00E00000u, 0x00200000u);                /* guest stacks and MEMORY_BASIC_INFORMATION */
    heap_init();
    if (argc > 2 && !strcmp(argv[1], "hc")){ planted(argv[2]); return 0; }
    threads(); realloc_zero(); virtual(); fragmentation(); heapcheck(argv[0]);
    printf("heap_stress: %s (%d failed)\n", fails ? "FAILED" : "passed", fails);
    return fails != 0;
}
