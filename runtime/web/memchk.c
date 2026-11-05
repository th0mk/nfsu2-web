/* wasm debug aid: guest_memchk for NFSU2_MEMCHECK builds (runtime/cpu.h).
 * Reports the first accesses outside the guest span with the lifted function
 * and the CPU registers, then traps like the unchecked build would. */
#include "cpu.h"
#include "../guest_span.h"
#include <stdio.h>
#include <stdlib.h>
extern CPU *g_cur_cpu;
uint8_t *guest_memchk(uint32_t a, unsigned n, const char *fn){
    if (guest_span(0x10000u, a, n)) return (uint8_t *)(uintptr_t)a;              /* (or the heap's window: guest_span.h) */
    static int shown = 0;
    if (shown++ < 16){
        CPU *c = g_cur_cpu;
        fprintf(stderr, "[memchk] %s: %u-byte access at 0x%08x outside the guest span", fn, n, a);
        if (c) fprintf(stderr, "  eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x",
                       c->eax, c->ebx, c->ecx, c->edx, c->esi, c->edi, c->ebp, c->esp);
        fprintf(stderr, "\n");
    }
    if (a < 0x10000u && (uint64_t)a + n <= GUEST_END) return (uint8_t *)(uintptr_t)a;   /* NULL page: report, continue */
    abort();
}
