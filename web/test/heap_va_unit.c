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
 *   dfree     a block freed into the free block before it: a second free of
 *             it is refused, it is not live for HeapSize/HeapValidate, and no
 *             block is handed out twice
 *   reserve   with the heaps' segments given back, 1.4 MB blocks filling the
 *             window leave the last segment's room (they take the rest of
 *             the room kept, a last resort), counted outside the kept holes;
 *             a 16 MB block may have it; 0x5CE170's texture buffer gets the
 *             scratch area when the heap has no room
 *   holes     a hole whose big block never comes back is kept ten minutes
 *             only; a block taking part of one of eight holes keeps the other
 *             seven (the oldest went although a slot was free); the pieces of
 *             a cut hole keep its age
 *   check     (NFSU2_HEAPCHECK=1, run with "check") a VirtualAlloc region
 *             released and reserved again reads zero, not the 0xDD poison;
 *             a new segment goes into released space, not into a hole
 *   trace     (every run) an FNV-1a of every address handed out: the same
 *             with the window asked for but refused (heap-va.test.mjs); with
 *             no window, the base heap.c's when the window came in (fa2cb5ce:
 *             main e5d58d5e's heap.c, the trace of these tests; a phone's
 *             start places every block where it did before)
 * With NFSU2_HEAP_WINDOW=1 and a 2 GB memory (WASI_PAGES=32768) the guest
 * heap's window above the runtime is on (runtime/guest_span.h) and these run
 * instead:
 *   win       the window is on; large blocks and VirtualAlloc go into it,
 *             heap segments stay low; nothing ever lands in the runtime's
 *             memory between; 1.4 MB blocks fill 1200 MB or more of the two
 *             (pages committed, written and read back), leaving a segment's
 *             room, then 0x5CE170's buffer gets the scratch area, and all of
 *             it comes back; VirtualQuery and a fixed-address VirtualAlloc see
 *             the hole as nobody's; a big block's hole in the window is kept
 *             and goes after ten minutes; a double free is refused
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
int gheap_kept_holes(uint32_t *, uint32_t *, int);
extern int g_gh_com, g_gh_gbuf;

/* heap.c's links into the rest of the runtime */
void guest_backtrace(CPU *c, int n){ (void)c; (void)n; }
long autotest_frame(void){ return 0; }
static uint64_t g_us;                                  /* the game's clock (win32.c host_us) */
uint64_t host_us(void){ return g_us; }
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
extern __attribute__((weak)) uint32_t g_gwin_lo, g_gwin_hi;   /* mem.c (none in a heap.c before the window) */
static uint32_t win_lo(void){ return &g_gwin_lo ? g_gwin_lo : 0; }
static uint32_t win_hi(void){ return &g_gwin_hi ? g_gwin_hi : 0; }

/* a Win32 heap call as the import bridge makes it: stdcall arguments at esp */
#define STK 0x00E0F000u
static uint32_t g_eip = 0x00401000u;                   /* the caller (its site) */
static uint32_t call(const char *fn, int n, const uint32_t *a){
    static CPU c;
    c.esp = STK;
    for (int i = n - 1; i >= 0; i--){ c.esp -= 4; MR32(c.esp) = a[i]; }
    c.eip = g_eip;
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

/* Two neighbours x, y (and the block after y live): x freed, then y, which
 * goes into x's free block.  Before, y's header still said live: a second
 * free of y was taken and y's block was in the free lists twice. */
enum { NDF = 64 };
static void dfree(void){
    uint32_t p[NDF], x = 0, y = 0;
    for (int i = 0; i < NDF; i++) p[i] = crt(100u);
    for (int i = 0; i < NDF && !x; i++) for (int j = 0; j < NDF && !x; j++) for (int k = 0; k < NDF && !x; k++)
        if (p[j] == p[i] + 128u && p[k] == p[j] + 128u){ x = p[i]; y = p[j]; }
    CHK("dfree-setup", x && y);
    crt_free(x); crt_free(y);
    CHK("dfree-not-live", H("HeapSize", g_h, 0u, y) == 0xFFFFFFFFu && H("HeapValidate", g_h, 0u, y) == 0u);
    CHK("dfree-refused", H("HeapFree", g_h, 0u, y) == 0u);
    uint32_t q[8]; int overlap = 0;
    for (int i = 0; i < 8; i++) q[i] = crt(100u);
    for (int i = 0; i < 8; i++){
        for (int j = i + 1; j < 8; j++) overlap += q[i] + 128u > q[j] && q[j] + 128u > q[i];
        for (int k = 0; k < NDF; k++) overlap += p[k] != x && p[k] != y && q[i] + 128u > p[k] && p[k] + 128u > q[i];
    }
    CHK("dfree-no-overlap", overlap == 0);
    for (int i = 0; i < 8; i++) crt_free(q[i]);
    for (int k = 0; k < NDF; k++) if (p[k] != x && p[k] != y) crt_free(p[k]);
}

/* 12 blocks of 900 KB (three segments) freed, the segments given back, the
 * window then filled with 1.4 MB blocks until they fail: they take the room
 * kept for segments too (a last resort, not a wall) but leave the last
 * segment's, where four more 900 KB blocks fit after the spare segment's
 * four (before the room, only those four fitted).  Then twelve 1.4 MB
 * blocks that lay together freed: a 16 MB block may take that room to its
 * last segment. */
enum { NFILL = 512 };
static uint32_t g_fill[NFILL];
static void reserve(void){
    uint32_t s[12], sm[64]; int nbig = 0, nsmall = 0, ok = 1;
    for (int i = 0; i < 12; i++) ok &= (s[i] = crt(900u << 10)) != 0;
    for (int i = 0; i < 12; i++) crt_free(s[i]);
    while (nbig < NFILL && (g_fill[nbig] = crt(1398016u))) nbig++;
    unsigned left = vrun();
    g_eip = 0x005ce1e9u;                                        /* 0x5CE170's malloc: the scratch area */
    uint32_t tb = crt(1398016u), tbig = crt(4u * MB);
    uint32_t tfree = H("HeapFree", g_h, 0u, tb);
    g_eip = 0x00401000u;
    CHK("scratch-texture-buffer", tb == 0x00010000u && tfree == 1u && tbig == 0u && crt(1398016u) == 0u);
    while (nsmall < 64 && (sm[nsmall] = crt(900u << 10))) nsmall++;
    printf("  window filled with %d blocks of 1.4 MB (the longest run left %u MB), then %d blocks of 900 KB\n", nbig, left, nsmall);
    CHK("reserve-last-segment", ok && nbig > 300 && nsmall >= 8);
    CHK("reserve-last-resort", left < 8);
    int k = -1;                                                 /* twelve that lie together: 16.5 MB */
    for (int i = nbig / 2; i + 11 < nbig && k < 0; i++){
        int adj = 1;
        for (int j2 = i; j2 < i + 11; j2++) adj &= g_fill[j2] == g_fill[j2 + 1] + 22u * 0x10000u;
        if (adj) k = i;
    }
    for (int j2 = k; k >= 0 && j2 < k + 12; j2++){ crt_free(g_fill[j2]); g_fill[j2] = 0; }
    uint32_t streamer = k >= 0 ? crt(16u * MB - 32u) : 0;
    CHK("reserve-big-not-held", k >= 0 && streamer != 0);
    crt_free(streamer);
    for (int i = 0; i < nsmall; i++) crt_free(sm[i]);
    for (int i = 0; i < nbig; i++) crt_free(g_fill[i]);
}

/* The kept holes (VirtualAlloc regions reserved at fixed addresses and
 * released make them where a test wants them). */
#define HOLE_TTL_US (600000ull * 1000ull)               /* heap.c HINT_TTL_MS */
static int hole_kept(uint32_t a){
    uint32_t ad[16], ln[16]; int n = gheap_kept_holes(ad, ln, 16);
    for (int i = 0; i < n; i++) if (ad[i] == a) return 1;
    return 0;
}
static int holes_n(void){ uint32_t ad[16], ln[16]; return gheap_kept_holes(ad, ln, 16); }
static uint32_t hole_at(uint32_t a, uint32_t n){
    uint32_t r = H("VirtualAlloc", a, n, 0x2000u, 4u);
    if (r == a) H("VirtualFree", a, 0u, 0x8000u);
    return r == a;
}
static void holes_expire(void){
    g_us += HOLE_TTL_US + 1000u;                               /* the other tests' holes go */
    uint32_t top = 0x28000000u - 20u * MB;                     /* the top of the window */
    int ok = holes_n() == 0 && hole_at(top, 20u * MB);
    uint32_t t = crt(1398016u);
    CHK("holes-kept", ok && hole_kept(top) && t && (t < top || t >= top + 20u * MB));
    g_us += HOLE_TTL_US / 2;
    CHK("holes-kept-a-while", hole_kept(top));
    g_us += HOLE_TTL_US / 2 + 1000u;
    uint32_t t2 = crt(1398016u);
    CHK("holes-expire", !hole_kept(top) && t2 >= top && t2 < top + 20u * MB);
    crt_free(t); crt_free(t2);
}
static void holes_compact(void){
    g_us += HOLE_TTL_US + 1000u;
    const uint32_t base = 0x1C000000u, step = 20u * MB + 0x10000u;
    uint32_t sep[8]; int ok = holes_n() == 0;
    for (int i = 0; i < 8; i++){                               /* 8 holes, each with a 64 KB region after it */
        uint32_t len = i == 7 ? 18u * MB : 20u * MB, a = base + (uint32_t)i * step;
        sep[i] = H("VirtualAlloc", a + len, 0x10000u, 0x2000u, 4u);
        ok &= sep[i] == a + len;
    }
    for (int i = 0; i < 8; i++){ g_us += 1000u; ok &= (int)hole_at(base + (uint32_t)i * step, i == 7 ? 18u * MB : 20u * MB); }
    ok &= holes_n() == 8;
    uint32_t x = crt(17u * MB);                                /* best fit: the 18 MB hole, the newest */
    ok &= x >= base + 7u * step && x < base + 7u * step + 18u * MB;
    CHK("holes-compact-setup", ok);
    CHK("holes-compact", holes_n() == 8 && hole_kept(base) && hole_kept(base + 7u * step));
    crt_free(x);
    for (int i = 0; i < 8; i++) H("VirtualFree", sep[i], 0u, 0x8000u);
}

/* Eight holes, the fourth cut in two by a region reserved in its middle: the
 * oldest hole goes for the second piece, and the pieces keep the fourth's
 * age, so three newer holes after that push out the first two holes and
 * then a piece, not the fifth (the pieces used to count as the newest). */
static void holes_age(void){
    g_us += HOLE_TTL_US + 1000u;
    const uint32_t base = 0x10000000u, step = 17u * MB;
    uint32_t sep[11]; int ok = holes_n() == 0;
    for (int i = 0; i < 11; i++){ sep[i] = H("VirtualAlloc", base + (uint32_t)i * step + 16u * MB, MB, 0x2000u, 4u);
                                  ok &= sep[i] == base + (uint32_t)i * step + 16u * MB; }
    for (int i = 0; i < 8; i++){ g_us += 1000u; ok &= (int)hole_at(base + (uint32_t)i * step, 16u * MB); }
    uint32_t mid = H("VirtualAlloc", base + 3u * step + 4u * MB, 4u * MB, 0x2000u, 4u);
    ok &= mid == base + 3u * step + 4u * MB;
    CHK("holes-age-setup", ok && holes_n() == 8 && !hole_kept(base) && hole_kept(base + 3u * step) && hole_kept(base + 3u * step + 8u * MB));
    for (int i = 8; i < 11; i++){ g_us += 1000u; hole_at(base + (uint32_t)i * step, 16u * MB); }
    int pieces = hole_kept(base + 3u * step) + hole_kept(base + 3u * step + 8u * MB);
    CHK("holes-age", holes_n() == 8 && hole_kept(base + 4u * step) && !hole_kept(base + step) && !hole_kept(base + 2u * step) && pieces == 1);
    H("VirtualFree", mid, 0u, 0x8000u);
    for (int i = 0; i < 11; i++) H("VirtualFree", sep[i], 0u, 0x8000u);
}

/* A big block's 60 MB hole kept: 1.4 MB blocks fill the space outside it
 * down to the last segment's room before one goes into the hole, and a new
 * heap's first segment finds that room (counted with the hole, the room
 * looked ample: the blocks filled all of the rest and the segment went into
 * the hole). */
static void reserve_holes(void){
    g_us += HOLE_TTL_US + 1000u;
    const uint32_t top = 0x28000000u - 60u * MB;
    int ok = holes_n() == 0 && hole_at(top, 60u * MB), n = 0, in = 0;
    while (!in && n < NFILL && (g_fill[n] = crt(1398016u))){ in = g_fill[n] >= top && g_fill[n] < top + 60u * MB; n++; }
    uint32_t h4 = H("HeapCreate", 0u, 0x1000u, 0u), p = H("HeapAlloc", h4, 0u, 100u);
    CHK("reserve-holes-setup", ok && in && h4 && p);
    CHK("reserve-outside-holes", p && (p < top || p >= top + 60u * MB));
    H("HeapFree", h4, 0u, p); H("HeapDestroy", h4);
    for (int i = 0; i < n; i++) crt_free(g_fill[i]);
}

/* the runtime's memory between the two ranges */
static int in_hole(uint32_t a, uint32_t n){ return (uint64_t)a + n > 0x28000000u && a < win_lo(); }
enum { NWFILL = 1200 };
static uint32_t g_wfill[NWFILL];
static void win(void){
    CHK("win-on", win_lo() == 0x50000000u && win_hi() == 0x7FFE0000u);
    uint32_t big = crt(1398016u), seg = gbuf(4000u), v = H("VirtualAlloc", 0u, 3u * MB, 0x3000u, 4u);
    CHK("win-large-high", big >= win_lo() && big < win_hi() && v >= win_lo() && v < win_hi());
    CHK("win-segments-low", seg >= 0x08000000u && seg < 0x28000000u);
    CHK("win-committed", mem_is_committed(big) && mem_is_committed(big + 1398015u) && mem_is_committed(v + 3u * MB - 1u));
    crt_free(big); gbuf_free(seg); H("VirtualFree", v, 0u, 0x8000u);
    uint64_t va0 = va(); uint32_t n = 0, hole = 0, bad = 0;
    while (n < NWFILL && (g_wfill[n] = crt(1398016u))){
        uint32_t p = g_wfill[n++];
        hole += in_hole(p, 1398016u);
        for (uint32_t o = 0; o < 1398016u; o += 65536u){ MR32(p + o) = p ^ o; }
    }
    for (uint32_t i = 0; i < n; i++) for (uint32_t o = 0; o < 1398016u; o += 65536u) bad += MR32(g_wfill[i] + o) != (g_wfill[i] ^ o);
    printf("  1.4 MB blocks until none: %u (%llu MB), %u in the runtime's memory, %u read back wrong\n",
           n, (unsigned long long)((uint64_t)n * 1398016u >> 20), hole, bad);
    CHK("win-fill", (uint64_t)n * 1398016u >= 1200ull * MB && n < NWFILL);
    CHK("win-no-hole", hole == 0 && bad == 0);
    g_eip = 0x005ce1e9u;                                        /* 0x5CE170's buffer: the scratch area */
    uint32_t tb = crt(1398016u);
    g_eip = 0x00401000u;
    uint32_t h5 = H("HeapCreate", 0u, 0x1000u, 0u), p5 = H("HeapAlloc", h5, 0u, 100u);   /* a new segment: the room left */
    CHK("win-scratch", tb == 0x00010000u && H("HeapFree", g_h, 0u, tb) == 1u);
    CHK("win-segment-room", h5 && p5);
    H("HeapFree", h5, 0u, p5); H("HeapDestroy", h5);
    for (uint32_t i = 0; i < n; i++) crt_free(g_wfill[i]);
    CHK("win-back", va() <= va0 + 4u * MB);
    /* the hole as the game sees it: free (nobody's), and no VirtualAlloc there */
    uint32_t mbi = 0x00E0E000u; mem_commit(mbi, 4096u);
    H("VirtualQuery", 0x30000000u, mbi, 28u);
    uint32_t at = H("VirtualAlloc", 0x30000000u, 1u * MB, 0x3000u, 4u);
    CHK("win-hole-nobodys", MR32(mbi + 16u) == 0x10000u && at == 0);
    H("VirtualQuery", 0x60000000u, mbi, 28u);
    CHK("win-query-free", MR32(mbi + 16u) == 0x10000u);
    /* a big block's hole in the window: kept (a 1.4 MB block goes past it), then gone after ten minutes */
    g_us += HOLE_TTL_US + 1000u;
    uint32_t top = 0x7FFE0000u - 20u * MB;
    int ok = holes_n() == 0 && hole_at(top, 20u * MB);
    uint32_t t = crt(1398016u);
    CHK("win-hole-kept", ok && hole_kept(top) && t && (t < top || t >= top + 20u * MB));
    g_us += HOLE_TTL_US + 1000u;
    uint32_t t2 = crt(1398016u);
    CHK("win-hole-expire", !hole_kept(top) && t2 >= top && t2 < top + 20u * MB);
    crt_free(t); crt_free(t2);
}

/* NFSU2_HEAPCHECK=1: released pages are 0xDD; a region reserved and
 * committed there again must read zero. */
static void check_zero(void){
    uint32_t a = H("VirtualAlloc", 0u, MB, 0x3000u, 4u);
    memset(g_mem + a, 0x5A, MB);
    H("VirtualFree", a, 0u, 0x8000u);
    uint32_t b = H("VirtualAlloc", a, MB, 0x3000u, 4u);
    int nz = 0;
    for (uint32_t i = 0; b && i < MB; i++) nz += MR8(b + i) != 0;
    CHK("check-release-zero", a && b == a && nz == 0);
    memset(g_mem + b, 0x5A, MB);
    H("VirtualFree", b, MB, 0x4000u);                           /* decommit, then commit again */
    uint32_t c = H("VirtualAlloc", b, MB, 0x1000u, 4u);
    nz = 0;
    for (uint32_t i = 0; c && i < MB; i++) nz += MR8(c + i) != 0;
    CHK("check-decommit-zero", c == b && nz == 0);
    H("VirtualFree", b, 0u, 0x8000u);
}
/* NFSU2_HEAPCHECK=1: a new segment goes into space released a while ago
 * rather than into a big block's hole that is cleaner.  The window: 4 MB in
 * 1 MB regions at the bottom, a 20 MB region, the rest reserved; the 20 MB
 * released (a hole), the released marks cleared by a request nothing had
 * room for, then the 4 MB released; then a new heap's first segment. */
static void check_hole(void){
    const uint32_t base = 0x08000000u;                          /* the window's bottom */
    uint32_t small[4], ok = 1, rest[64]; int nr = 0;
    for (int i = 0; i < 4; i++) ok &= (small[i] = H("VirtualAlloc", base + (uint32_t)i * MB, MB, 0x2000u, 4u)) == base + (uint32_t)i * MB;
    uint32_t hole = H("VirtualAlloc", base + 4u * MB, 20u * MB, 0x2000u, 4u);
    ok &= hole == base + 4u * MB;
    for (uint32_t a = base + 24u * MB; a < 0x28000000u && nr < 64; a += 8u * MB) ok &= (rest[nr++] = H("VirtualAlloc", a, 8u * MB, 0x2000u, 4u)) == a;
    H("VirtualFree", hole, 0u, 0x8000u);
    ok &= H("VirtualAlloc", 0u, 30u * MB, 0x2000u, 4u) == 0u;   /* no room: the released marks go */
    for (int i = 0; i < 4; i++) H("VirtualFree", small[i], 0u, 0x8000u);
    uint32_t h3 = H("HeapCreate", 0u, 0x1000u, 0u), p = H("HeapAlloc", h3, 0u, 100u);
    CHK("check-hole-setup", ok && h3 && p);
    CHK("check-hole-kept", p >= base && p < base + 4u * MB);
    H("HeapFree", h3, 0u, p); H("HeapDestroy", h3);
    for (int i = 0; i < nr; i++) H("VirtualFree", rest[i], 0u, 0x8000u);
}

int main(int argc, char **argv){
    mem_init();
    mem_commit(0x00E00000u, 0x00010000u);                /* the calls' guest stack */
    heap_init();
    g_h = H("HeapCreate", 0u, 0x1000u, 0u);
    if (argc > 1 && !strcmp(argv[1], "check")){ check_zero(); check_hole(); }
    else if (win_hi()){
        win(); dfree();
        g_ncar = 64; g_carpool = 131072000u + 172u;
        trips();
    }
    else { bestfit(); bighole(); racehole(); segments(); trips(); dfree(); reserve(); holes_expire(); holes_compact(); holes_age(); reserve_holes(); }
    printf("trace %08x\n", g_trace);
    printf("heap_va_unit: %s (%d failed)\n", fails ? "FAILED" : "passed", fails);
    return fails != 0;
}
