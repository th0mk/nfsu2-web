/* heap_va_unit.c -- runtime/heap.c's address space on wasm (the browser's 512 MB
 * guest heap window) over what a long session in a crowded free roam does to
 * it: each trip out of the world and back gives back and takes again the track
 * streamer's 60000 KB block (the game's main pool grows by exactly that when it
 * has no such free block), friends' cars come and go in 1.4 MB blocks, and the
 * COM buffers' heap swings by a hundred MB of textures between the world and
 * the front end.  Built and run by web/test/heap-va.test.mjs.
 *   bestfit   a 1.4 MB block goes into a 2 MB hole, not into the 60 MB one the
 *             streamer left, and a 59 MB block then gets that one whole
 *   bighole   with no small hole, 1.4 MB blocks and heap segments go to the
 *             wide free run, and the streamer's block comes back into its
 *             own hole
 *   racehole  a race track's smaller streamer block in that hole for a
 *             while: the rest of the hole is kept too, and the 60000 KB
 *             block gets it whole again after the race
 *   segments  a heap that held 160 MB of small blocks gives that address space
 *             back once they are freed (one 4 MB segment kept for its next
 *             growth), and holds only what a few long-lived blocks pin
 *   trips     60 trips out of a crowded world and back: no allocation fails,
 *             the address space stays bounded, and a 60 MB run is free at
 *             every trip back
 *   trace     (every run) an FNV-1a of every address handed out: the same
 *             with the window asked for but refused (heap-va.test.mjs); with
 *             no window it was the base heap.c's when the window came in
 *             (3726865a for track/heapva ee93ad06's heap.c: a phone's start
 *             places every block where it did before)
 * With NFSU2_HEAP_WINDOW=1 and a 2 GB memory (WASI_PAGES=32768) the guest
 * heap's window above the runtime is on (runtime/guest_span.h) and these run
 * instead:
 *   win       the window is on; large blocks and VirtualAlloc go into it,
 *             heap segments stay low; nothing ever lands in the runtime's
 *             memory between; 1.4 MB blocks fill 1200 MB or more of the two
 *             (pages committed, written and read back), and all of it comes
 *             back; VirtualQuery and a fixed-address VirtualAlloc see the hole
 *             as nobody's
 *   wtrips    60 trips with 64 friends' cars and the car pool at 128000 KB:
 *             no allocation fails and the streamer's run is always there */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mem_init(void); void mem_commit(uint32_t, uint32_t);
void heap_init(void);
int heap_api_kind(const char *); uint32_t heap_api(CPU *, int);
uint32_t gheap_alloc_c(CPU *, int, uint32_t, int); void gheap_free_c(CPU *, int, uint32_t);
void heap_totals(uint64_t *, uint64_t *, uint64_t *, uint64_t *);
int memstat_line(char *, size_t);
extern int g_gh_com, g_gh_gbuf;

/* heap.c's links into the rest of the runtime */
void guest_backtrace(CPU *c, int n){ (void)c; (void)n; }
long autotest_frame(void){ return 0; }
void win32_set_last_error(CPU *c, uint32_t e){ (void)c; (void)e; }
void com_mem_stats(uint64_t *a, uint64_t *b, uint64_t *o){ *a = *b = *o = 0; }
void metal_mem_stats(uint64_t *a, unsigned *l, unsigned *t, uint64_t *b){ *a = 0; *l = *t = 0; *b = 0; }
int win32_obj_live(void){ return 0; }
int fileio_open_handles(void){ return 0; }
int win32_threads_live(void){ return 0; }

static int fails = 0;
#define CHK(name, cond) do{ if (cond) printf("PASS %s\n", name); else { printf("FAIL %s\n", name); fails++; } }while(0)
/* every address handed out, in order (the trace's FNV-1a) */
static uint32_t g_trace = 0x811C9DC5u;
static uint32_t traced(uint32_t a){ for (int i = 0; i < 4; i++){ g_trace ^= (a >> (8 * i)) & 0xffu; g_trace *= 0x01000193u; } return a; }
extern __attribute__((weak)) uint32_t g_gwin_lo, g_gwin_hi;   /* mem.c (none in the base heap.c's) */
static uint32_t win_lo(void){ return &g_gwin_lo ? g_gwin_lo : 0; }
static uint32_t win_hi(void){ return &g_gwin_hi ? g_gwin_hi : 0; }

/* a Win32 heap call as the import bridge makes it: stdcall arguments at esp */
#define STK 0x00E0F000u
static uint32_t call(const char *fn, int n, const uint32_t *a){
    static CPU c;
    c.esp = STK;
    for (int i = n - 1; i >= 0; i--){ c.esp -= 4; MR32(c.esp) = a[i]; }
    c.eip = 0x00401000u;
    return heap_api(&c, heap_api_kind(fn));
}
#define H(fn, ...) ({ uint32_t _a[] = { __VA_ARGS__ }; call(fn, (int)(sizeof _a / 4), _a); })
static uint32_t rnd(uint32_t *s){ *s = *s * 1103515245u + 12345u; return *s >> 7; }
static uint32_t g_h;                                   /* the game's CRT heap (HeapCreate) */
static uint32_t crt(uint32_t n){ return traced(H("HeapAlloc", g_h, 0u, n)); }
static void crt_free(uint32_t p){ if (p) H("HeapFree", g_h, 0u, p); }
static CPU g_c;
static uint32_t gbuf(uint32_t n){ return traced(gheap_alloc_c(&g_c, g_gh_gbuf, n, 0)); }
static void gbuf_free(uint32_t p){ if (p) gheap_free_c(&g_c, g_gh_gbuf, p); }
static uint64_t va(void){ uint64_t v; heap_totals(0, 0, &v, 0); return v; }
/* the longest free run of the address space, MB (the [status] line's vrun=) */
static unsigned vrun(void){
    char b[512]; memstat_line(b, sizeof b);
    const char *p = strstr(b, " vrun="); return p ? (unsigned)atoi(p + 6) : 0;
}
#define MB (1u << 20)

static void bestfit(void){
    uint32_t big = crt(60u * MB), sep1 = crt(1398016u), mid = crt(2u * MB), sep2 = crt(1398016u);
    CHK("bestfit-setup", big && sep1 && mid && sep2 && big > sep1 && sep1 > mid && mid > sep2);
    crt_free(big); crt_free(mid);
    uint32_t p = crt(1398016u);
    CHK("bestfit-small-hole", p >= mid && p < mid + 2u * MB);
    uint32_t q = crt(59u * MB);
    CHK("bestfit-big-hole", q >= big && q < big + 60u * MB);
    crt_free(p); crt_free(q); crt_free(sep1); crt_free(sep2);
}

/* The streamer's block given back at a trip out: the textures and heap
 * segments that come meanwhile go elsewhere, and its block comes back whole
 * into its own hole. */
enum { NTEX = 30, NSEG_SMALL = 4000 };
static uint32_t g_tex[NTEX], g_small[NSEG_SMALL];
static void bighole(void){
    uint32_t a = crt(61440172u), sep = crt(1398016u), s = 7;
    CHK("bighole-setup", a && sep && sep < a);
    crt_free(a);
    int ok = 1;
    for (int i = 0; i < NTEX; i++) ok &= (g_tex[i] = crt(1398016u)) != 0;
    for (int i = 0; i < NSEG_SMALL; i++) ok &= (g_small[i] = gbuf(1000u + rnd(&s) % 4000u)) != 0;
    int inside = 0;
    for (int i = 0; i < NTEX; i++) inside += g_tex[i] >= a && g_tex[i] < a + 61440172u;
    for (int i = 0; i < NSEG_SMALL; i++) inside += g_small[i] >= a && g_small[i] < a + 61440172u;
    CHK("bighole-untouched", ok && !inside);
    uint32_t again = crt(61440172u);
    CHK("bighole-back", again == a);
    crt_free(again); crt_free(sep);
    for (int i = 0; i < NTEX; i++) crt_free(g_tex[i]);
    for (int i = 0; i < NSEG_SMALL; i++) gbuf_free(g_small[i]);
}

/* A race on another track: its streamer's 36000 KB goes into free roam's
 * 60000 KB hole for the race, the textures meanwhile go past the rest of that
 * hole, and back in free roam its block finds the hole whole again. */
static void racehole(void){
    uint32_t a = crt(61440172u), sep = crt(1398016u), s = 11;
    crt_free(a);
    uint32_t race = crt(36864172u);
    CHK("racehole-in-hole", race >= a && race < a + 61440172u);
    int inside = 0;
    for (int i = 0; i < NTEX; i++){ g_tex[i] = crt(1398016u); inside += g_tex[i] >= a && g_tex[i] < a + 61440172u; }
    for (int i = 0; i < NSEG_SMALL; i++){ g_small[i] = gbuf(1000u + rnd(&s) % 4000u); inside += g_small[i] >= a && g_small[i] < a + 61440172u; }
    CHK("racehole-rest-untouched", !inside);
    crt_free(race);
    uint32_t again = crt(61440172u);
    CHK("racehole-back", again == a);
    crt_free(again); crt_free(sep);
    for (int i = 0; i < NTEX; i++) crt_free(g_tex[i]);
    for (int i = 0; i < NSEG_SMALL; i++) gbuf_free(g_small[i]);
}

enum { NSMALL = 65536 };
static uint32_t g_p[NSMALL];
static void segments(void){
    uint64_t va0 = va(); uint32_t s = 99;
    int ok = 1;
    for (int i = 0; i < NSMALL; i++) ok &= (g_p[i] = gbuf(1000u + rnd(&s) % 3000u)) != 0;
    uint64_t va1 = va();
    for (int i = 0; i < NSMALL; i++) gbuf_free(g_p[i]);
    uint64_t va2 = va();
    printf("  160 MB of small blocks: VA %llu MB before, %llu MB with them, %llu MB after\n",
           (unsigned long long)(va0 >> 20), (unsigned long long)(va1 >> 20), (unsigned long long)(va2 >> 20));
    CHK("segments-alloc", ok && va1 >= va0 + 120u * MB);
    CHK("segments-back", va2 <= va0 + 4u * MB);
    for (int i = 0; i < NSMALL; i++) g_p[i] = gbuf(1000u + rnd(&s) % 3000u);
    for (int i = 1000; i < NSMALL; i++) gbuf_free(g_p[i]);         /* the first 1000 stay: a segment or two */
    uint64_t va3 = va();
    CHK("segments-pinned", va3 <= va0 + 12u * MB);
    uint32_t x = gbuf(3000u);                                        /* room left: no new segment */
    CHK("segments-reuse", x && va() == va3);
    gbuf_free(x);
    for (int i = 0; i < 1000; i++) gbuf_free(g_p[i]);
    CHK("segments-empty", va() <= va0 + 4u * MB);
}

/* A session: the main pool's first blocks and the car pool's extra block for
 * good; each trip back the streamer's block, the world's textures (small
 * blocks in the COM buffers' heap, some kept for longer: shared packs), 48
 * friends' cars (a 1.4 MB block in each heap and their small textures) coming
 * and going; each trip out all that goes and the front end's textures come. */
enum { NCAR = 64, NWORLD = 3000, NFE = 2600, NKEEP = 200, TRIPS = 60 };
static int g_ncar = 48;                                 /* friends' cars (64 in wtrips) */
static uint32_t g_carpool = 64512172u;                  /* the car pool's extra block (128000 KB's in wtrips) */
static uint32_t g_world[NWORLD], g_wcrt[NWORLD], g_fe[NFE], g_fecrt[NFE], g_keep[NKEEP];
static uint32_t g_car[NCAR][2], g_cartex[NCAR][40];
static void car_in(int k, uint32_t *s){
    g_car[k][0] = crt(1398016u); g_car[k][1] = gbuf(1398016u);
    for (int t = 0; t < 40; t++) g_cartex[k][t] = gbuf(8192u + rnd(s) % 57344u);
}
static void car_out(int k){
    crt_free(g_car[k][0]); gbuf_free(g_car[k][1]); g_car[k][0] = g_car[k][1] = 0;
    for (int t = 0; t < 40; t++){ gbuf_free(g_cartex[k][t]); g_cartex[k][t] = 0; }
}
static void trips(void){
    uint32_t s = 4242, nkeep = 0;
    uint32_t pool1 = crt(9228288u), pool2 = crt(8073216u), carpool = crt(g_carpool);
    int failed = !pool1 || !pool2 || !carpool;
    uint64_t vmax = 0; unsigned minrun = ~0u;
    for (int trip = 0; trip < TRIPS; trip++){
        for (int i = 0; i < NFE; i++){ gbuf_free(g_fe[i]); crt_free(g_fecrt[i]); g_fe[i] = g_fecrt[i] = 0; }
        unsigned r = vrun(); if (r < minrun) minrun = r;
        uint32_t streamer = crt(61440172u);
        failed += !streamer;
        for (int i = 0; i < NWORLD; i++){
            failed += !(g_world[i] = gbuf(1024u + rnd(&s) % 61440u));
            failed += !(g_wcrt[i] = crt(16u + rnd(&s) % 4000u));
        }
        for (int k = 0; k < g_ncar; k++) car_in(k, &s);
        for (int round = 0; round < 6; round++)                      /* friends come and go */
            for (int k = 0; k < g_ncar; k++) if (rnd(&s) & 1){ car_out(k); car_in(k, &s); }
        for (int k = 0; k < g_ncar; k++) failed += !g_car[k][0] || !g_car[k][1] || !g_cartex[k][39];
        if (va() > vmax) vmax = va();
        crt_free(streamer);                                          /* the track unload */
        for (int k = 0; k < g_ncar; k++) car_out(k);
        for (int i = 0; i < NWORLD; i++){
            if (!(rnd(&s) % 300) && nkeep < NKEEP) g_keep[nkeep++] = g_world[i];   /* kept for longer */
            else gbuf_free(g_world[i]);
            crt_free(g_wcrt[i]);
        }
        if (nkeep >= NKEEP / 2){ gbuf_free(g_keep[0]); memmove(g_keep, g_keep + 1, --nkeep * 4u); }
        for (int i = 0; i < NFE; i++){                               /* the front end */
            failed += !(g_fe[i] = gbuf(1024u + rnd(&s) % 70000u));
            failed += !(g_fecrt[i] = crt(16u + rnd(&s) % 4000u));
        }
        if (va() > vmax) vmax = va();
    }
    printf("  %d trips, %d cars: VA at most %llu MB, the longest free run at a trip back at least %u MB, %d failed\n",
           TRIPS, g_ncar, (unsigned long long)(vmax >> 20), minrun, failed);
    if (win_hi()){
        CHK("wtrips-no-failure", failed == 0);
        CHK("wtrips-streamer-room", minrun >= 60u);
    } else {
        CHK("trips-no-failure", failed == 0);
        CHK("trips-bounded", vmax < 480ull * MB);
        CHK("trips-streamer-room", minrun >= 60u);
    }
    for (int i = 0; i < NFE; i++){ gbuf_free(g_fe[i]); crt_free(g_fecrt[i]); }
    for (uint32_t i = 0; i < nkeep; i++) gbuf_free(g_keep[i]);
    crt_free(pool1); crt_free(pool2); crt_free(carpool);
}

/* the runtime's memory between the two ranges */
static int in_hole(uint32_t a, uint32_t n){ return (uint64_t)a + n > 0x28000000u && a < win_lo(); }
enum { NFILL = 1200 };
static uint32_t g_fill[NFILL];
static void win(void){
    CHK("win-on", win_lo() == 0x50000000u && win_hi() == 0x7FFE0000u);
    uint32_t big = crt(1398016u), seg = gbuf(4000u), v = H("VirtualAlloc", 0u, 3u * MB, 0x3000u, 4u);
    CHK("win-large-high", big >= win_lo() && big < win_hi() && v >= win_lo() && v < win_hi());
    CHK("win-segments-low", seg >= 0x08000000u && seg < 0x28000000u);
    CHK("win-committed", mem_is_committed(big) && mem_is_committed(big + 1398015u) && mem_is_committed(v + 3u * MB - 1u));
    crt_free(big); gbuf_free(seg); H("VirtualFree", v, 0u, 0x8000u);
    uint64_t va0 = va(); uint32_t n = 0, hole = 0, bad = 0;
    while (n < NFILL && (g_fill[n] = crt(1398016u))){
        uint32_t p = g_fill[n++];
        hole += in_hole(p, 1398016u);
        for (uint32_t o = 0; o < 1398016u; o += 65536u){ MR32(p + o) = p ^ o; }
    }
    for (uint32_t i = 0; i < n; i++) for (uint32_t o = 0; o < 1398016u; o += 65536u) bad += MR32(g_fill[i] + o) != (g_fill[i] ^ o);
    printf("  1.4 MB blocks until none: %u (%llu MB), %u in the runtime's memory, %u read back wrong\n",
           n, (unsigned long long)((uint64_t)n * 1398016u >> 20), hole, bad);
    CHK("win-fill", (uint64_t)n * 1398016u >= 1200ull * MB && n < NFILL);
    CHK("win-no-hole", hole == 0 && bad == 0);
    for (uint32_t i = 0; i < n; i++) crt_free(g_fill[i]);
    CHK("win-back", va() <= va0 + 4u * MB);
    /* the hole as the game sees it: free (nobody's), and no VirtualAlloc there */
    uint32_t mbi = 0x00E0E000u; mem_commit(mbi, 4096u);
    H("VirtualQuery", 0x30000000u, mbi, 28u);
    uint32_t at = H("VirtualAlloc", 0x30000000u, 1u * MB, 0x3000u, 4u);
    CHK("win-hole-nobodys", MR32(mbi + 16u) == 0x10000u && at == 0);
    H("VirtualQuery", 0x60000000u, mbi, 28u);
    CHK("win-query-free", MR32(mbi + 16u) == 0x10000u);
}

int main(void){
    mem_init();
    mem_commit(0x00E00000u, 0x00010000u);                /* the calls' guest stack */
    heap_init();
    g_h = H("HeapCreate", 0u, 0x1000u, 0u);
    if (win_hi()){
        win();
        g_ncar = 64; g_carpool = 131072000u + 172u;
        trips();
    } else {
        bestfit(); bighole(); racehole(); segments(); trips();
    }
    printf("trace %08x\n", g_trace);
    printf("heap_va_unit: %s (%d failed)\n", fails ? "FAILED" : "passed", fails);
    return fails != 0;
}
