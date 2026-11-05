/* sbrk_window_unit.c -- runtime/web/sbrk.c with the guest heap's window
 * (runtime/guest_span.h): the runtime's heap (wasi-libc's dlmalloc on this
 * sbrk) steps over the window and never hands out a byte of it.  The window
 * is put just above the break here (the game's is at 1280 MB, past what the
 * runtime ever used), so the heap has to step over it: blocks of every size
 * until the heap is far past the window, written and read back, freed and
 * taken again.  Built and run by web/test/heap-va.test.mjs. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t g_gwin_lo = 0, g_gwin_hi = 0;      /* (mem.c's, in the game) */
uint64_t heap_break(void);

static int fails = 0;
#define CHK(name, cond) do{ if (cond) printf("PASS %s\n", name); else { printf("FAIL %s\n", name); fails++; } }while(0)

enum { N = 6000 };
static uint8_t *g_p[N]; static uint32_t g_n[N];

int main(void){
    void *first = malloc(1000);                                  /* dlmalloc's first segment, below the window */
    uint64_t b0 = heap_break();
    g_gwin_lo = (uint32_t)((b0 + (64u << 20)) & ~0xFFFFu);       /* a 192 MB window 64 MB above the break */
    g_gwin_hi = g_gwin_lo + (192u << 20);
    printf("  break %llu MB, window %u..%u MB\n", (unsigned long long)(b0 >> 20), g_gwin_lo >> 20, g_gwin_hi >> 20);
    uint32_t s = 12345, in = 0, bad = 0;
    uint64_t total = 0;
    for (int i = 0; i < N; i++){
        s = s * 1103515245u + 12345u;
        uint32_t n = (s >> 8) % 20 == 0 ? (1u << 20) + (s >> 12) % (3u << 20) : 16u + (s >> 10) % 60000u;   /* some big ones */
        g_p[i] = malloc(n); g_n[i] = n; total += n;
        if (!g_p[i]){ printf("  malloc %u failed at %d\n", n, i); bad++; break; }
        uint64_t a = (uintptr_t)g_p[i];
        in += a + n > g_gwin_lo && a < g_gwin_hi;
        memset(g_p[i], (int)(i & 0xff), n);
    }
    for (int i = 0; i < N && g_p[i]; i++)
        for (uint32_t o = 0; o < g_n[i]; o += 997u) bad += g_p[i][o] != (uint8_t)(i & 0xff);
    uint64_t b1 = heap_break();
    printf("  %llu MB in %d blocks: break %llu MB, %u in the window, %u read back wrong\n",
           (unsigned long long)(total >> 20), N, (unsigned long long)(b1 >> 20), in, bad);
    CHK("sbrk-past-window", b1 > g_gwin_hi);
    CHK("sbrk-none-in-window", in == 0 && bad == 0);
    for (int i = 0; i < N; i += 2){ free(g_p[i]); g_p[i] = 0; }
    in = 0;
    for (int i = 0; i < N; i += 2){
        uint32_t n = g_n[(i + 1) % N] ? g_n[(i + 1) % N] : 4096u;
        g_p[i] = malloc(n); g_n[i] = n;
        uint64_t a = (uintptr_t)g_p[i];
        in += g_p[i] && a + n > g_gwin_lo && a < g_gwin_hi;
        if (g_p[i]) memset(g_p[i], (int)(i & 0xff), n);
    }
    for (int i = 0; i < N; i++) if (g_p[i]) for (uint32_t o = 0; o < g_n[i]; o += 997u) bad += g_p[i][o] != (uint8_t)(i & 0xff);
    CHK("sbrk-reuse", in == 0 && bad == 0);
    free(first);
    printf("sbrk_window_unit: %s (%d failed)\n", fails ? "FAILED" : "passed", fails);
    return fails != 0;
}
