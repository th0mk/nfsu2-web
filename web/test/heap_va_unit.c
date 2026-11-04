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
 *             window leave room for new segments: as many 900 KB blocks fit
 *             after them as had fitted before
 *   holes     a hole whose big block never comes back is kept HINT_TTL frames
 *             only; a block taking part of one of eight holes keeps the other
 *             seven (the oldest went although a slot was free)
 *   check     (NFSU2_HEAPCHECK=1, run with "check") a VirtualAlloc region
 *             released and reserved again reads zero, not the 0xDD poison */
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
static long g_frame;                                   /* the game's frame count */
long autotest_frame(void){ return g_frame; }
void win32_set_last_error(CPU *c, uint32_t e){ (void)c; (void)e; }
void com_mem_stats(uint64_t *a, uint64_t *b, uint64_t *o){ *a = *b = *o = 0; }
void metal_mem_stats(uint64_t *a, unsigned *l, unsigned *t, uint64_t *b){ *a = 0; *l = *t = 0; *b = 0; }
int win32_obj_live(void){ return 0; }
int fileio_open_handles(void){ return 0; }
int win32_threads_live(void){ return 0; }

static int fails = 0;
#define CHK(name, cond) do{ if (cond) printf("PASS %s\n", name); else { printf("FAIL %s\n", name); fails++; } }while(0)

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
static uint32_t crt(uint32_t n){ return H("HeapAlloc", g_h, 0u, n); }
static void crt_free(uint32_t p){ if (p) H("HeapFree", g_h, 0u, p); }
static CPU g_c;
static uint32_t gbuf(uint32_t n){ return gheap_alloc_c(&g_c, g_gh_gbuf, n, 0); }
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
enum { NCAR = 48, NWORLD = 3000, NFE = 2600, NKEEP = 200, TRIPS = 60 };
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
    uint32_t pool1 = crt(9228288u), pool2 = crt(8073216u), carpool = crt(64512172u);
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
        for (int k = 0; k < NCAR; k++) car_in(k, &s);
        for (int round = 0; round < 6; round++)                      /* friends come and go */
            for (int k = 0; k < NCAR; k++) if (rnd(&s) & 1){ car_out(k); car_in(k, &s); }
        for (int k = 0; k < NCAR; k++) failed += !g_car[k][0] || !g_car[k][1] || !g_cartex[k][39];
        if (va() > vmax) vmax = va();
        crt_free(streamer);                                          /* the track unload */
        for (int k = 0; k < NCAR; k++) car_out(k);
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
    printf("  %d trips: VA at most %llu MB, the longest free run at a trip back at least %u MB, %d failed\n",
           TRIPS, (unsigned long long)(vmax >> 20), minrun, failed);
    CHK("trips-no-failure", failed == 0);
    CHK("trips-bounded", vmax < 480ull * MB);
    CHK("trips-streamer-room", minrun >= 60u);
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
 * window then filled with 1.4 MB blocks: the 900 KB blocks still find new
 * segments (before, the 1.4 MB blocks took all of it and only the one spare
 * segment's 4 were left). */
enum { NFILL = 512 };
static uint32_t g_fill[NFILL];
static void reserve(void){
    uint32_t s[12], sm[64]; int nbig = 0, nsmall = 0, ok = 1;
    for (int i = 0; i < 12; i++) ok &= (s[i] = crt(900u << 10)) != 0;
    for (int i = 0; i < 12; i++) crt_free(s[i]);
    while (nbig < NFILL && (g_fill[nbig] = crt(1398016u))) nbig++;
    while (nsmall < 64 && (sm[nsmall] = crt(900u << 10))) nsmall++;
    printf("  window filled with %d blocks of 1.4 MB, then %d blocks of 900 KB\n", nbig, nsmall);
    CHK("reserve-small-after-big", ok && nbig > 300 && nsmall >= 12);
    for (int i = 0; i < nsmall; i++) crt_free(sm[i]);
    for (int i = 0; i < nbig; i++) crt_free(g_fill[i]);
}

/* The kept holes (VirtualAlloc regions reserved at fixed addresses and
 * released make them where a test wants them). */
#define HOLE_TTL 36000
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
    g_frame += HOLE_TTL + 1;                                   /* the other tests' holes go */
    uint32_t top = 0x28000000u - 20u * MB;                     /* the top of the window */
    int ok = holes_n() == 0 && hole_at(top, 20u * MB);
    uint32_t t = crt(1398016u);
    CHK("holes-kept", ok && hole_kept(top) && t && (t < top || t >= top + 20u * MB));
    g_frame += HOLE_TTL / 2;
    CHK("holes-kept-a-while", hole_kept(top));
    g_frame += HOLE_TTL / 2 + 1;
    uint32_t t2 = crt(1398016u);
    CHK("holes-expire", !hole_kept(top) && t2 >= top && t2 < top + 20u * MB);
    crt_free(t); crt_free(t2);
}
static void holes_compact(void){
    g_frame += HOLE_TTL + 1;
    const uint32_t base = 0x1C000000u, step = 20u * MB + 0x10000u;
    uint32_t sep[8]; int ok = holes_n() == 0;
    for (int i = 0; i < 8; i++){                               /* 8 holes, each with a 64 KB region after it */
        uint32_t len = i == 7 ? 18u * MB : 20u * MB, a = base + (uint32_t)i * step;
        sep[i] = H("VirtualAlloc", a + len, 0x10000u, 0x2000u, 4u);
        ok &= sep[i] == a + len;
    }
    for (int i = 0; i < 8; i++){ g_frame++; ok &= (int)hole_at(base + (uint32_t)i * step, i == 7 ? 18u * MB : 20u * MB); }
    ok &= holes_n() == 8;
    uint32_t x = crt(17u * MB);                                /* best fit: the 18 MB hole, the newest */
    ok &= x >= base + 7u * step && x < base + 7u * step + 18u * MB;
    CHK("holes-compact-setup", ok);
    CHK("holes-compact", holes_n() == 8 && hole_kept(base) && hole_kept(base + 7u * step));
    crt_free(x);
    for (int i = 0; i < 8; i++) H("VirtualFree", sep[i], 0u, 0x8000u);
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

int main(int argc, char **argv){
    mem_init();
    mem_commit(0x00E00000u, 0x00010000u);                /* the calls' guest stack */
    heap_init();
    g_h = H("HeapCreate", 0u, 0x1000u, 0u);
    if (argc > 1 && !strcmp(argv[1], "check")) check_zero();
    else { bestfit(); bighole(); racehole(); segments(); trips(); dfree(); reserve(); holes_expire(); holes_compact(); }
    printf("heap_va_unit: %s (%d failed)\n", fails ? "FAILED" : "passed", fails);
    return fails != 0;
}
