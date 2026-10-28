/* fxoracle.c -- the dev oracle for the FXLC preshader evaluator (M5-03,
 * docs/mw/fx-preshader.md §5.2, §6): Microsoft's d3dx9_26.dll under Unicorn.
 *
 *   tools/mw/fxoracle.sh [--quick]      (builds it, writes the cases, runs A and it, compares)
 *   fxoracle [--dll <d3dx9_26.dll>] [--conf games/mw/game.conf] [--verbose] [--features] vm <cases.txt> <out.txt|->
 *   fxoracle [...] [--begin-flags <n>] effect <rounds.txt> <out.txt|-> <NAME>.fxo...
 *
 * Test-only binary: Unicorn is GPLv2, and the DLL is a local dev oracle
 * (GAME.md §5.1).  Neither is ever linked into the game or the app, and
 * nothing of the DLL is written here: its SHA-256 must equal game.conf's
 * D3DX_SHA256 (NFSMW_D3DX or --dll names it), and the addresses below are
 * the only facts taken from it (fx-preshader §1).
 *
 * The DLL is mapped at its preferred base 0x00400000 (no relocation) and
 * DllMain runs.  Its imports are bound to addresses that are not mapped: a
 * call stops the emulator and the host implements it (msvcrt's heap,
 * strings, _controlfp on the emulated x87 control word, qsort calling the
 * guest comparator; kernel32's heap, locks and version queries; registry
 * lookups that find nothing; IsProcessorFeaturePresent says no, or with
 * --features yes to SSE and SSE2), or the run fails naming the import.  floor
 * and the _CI* helpers, which take and return x87 registers, go through a
 * few bytes of guest code that move the operands to the stack and load the
 * host's result after; _initterm is guest code too.  The host's results
 * are the C library's (fx-preshader §5.2).
 *
 * vm:     the interpreter (0x4E8817, stdcall, 14 arguments) in isolation,
 *         after the loader's check (0x4E9146), on every case of an
 *         "fxpcase 1" file at the x87 precision it names.  The tables and a
 *         copy of the blob each end where an unmapped page begins, the 256
 *         input slots start at an unmapped address, the temporaries at 0,
 *         and the run stops at the first access outside the case's memory
 *         (a fault, a data access in the interpreter's own frame, the 257th
 *         input's slot, the noise function); the loader's check stops at
 *         the first read past the blob that decides its verdict (run_vm
 *         says which).  Writes "fxpres 1 oracle" (§6.2).
 * effect: D3DXCreateEffectPool and D3DXCreateEffect on every effect file
 *         (byte order of NAME, one pool) with a recording IDirect3DDevice9,
 *         the SetValue rounds of an "fxpround 1" file through the vtable,
 *         with its typed setter calls at the round's control word, then
 *         every pass of every technique: SetTechnique, Begin (flags
 *         D3DXFX_DONOTSAVESTATE unless --begin-flags), BeginPass at the
 *         round's control word, EndPass, End.  Writes each setter's result,
 *         the render, stage and sampler states, transforms, materials and
 *         lights each BeginPass set, and the control words the interpreter
 *         ran at ("fxpdev 1", §6.3).
 */
#include "fx2.h"
#include "fxtext.h"
#include "../../runtime/d3d9_tables.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>

/* ---- the DLL (d3dx9_26.dll, 9.07.239.0000; game.conf's D3DX_SHA256) ---- */
#define DLL_BASE   0x00400000u
#define DLL_SIZE   0x0024A000u
#define VA_FXLCVM  0x004E8817u     /* the FXLC interpreter, stdcall, 14 arguments (fx-preshader §3.1) */

/* ---- the guest address space ---- */
#define HEAP       0x10000000u     /* malloc and friends: a bump allocator */
#define HEAPSZ     0x08000000u
#define SCR        0x20000000u     /* the vm mode's tables */
#define SCRSZ      0x00100000u
#define STK        0x28000000u
#define STKSZ      0x00100000u
#define TEB        0x2F000000u     /* fs: the TIB (SEH chain at +0) */
#define GDT_VA     0x2F010000u
#define STUBS      0x2F020000u     /* guest code for the x87 imports, and the fake objects' vtables */
#define HOSTCALL   0x7E000000u     /* host functions: HOSTCALL + 16 k, never mapped */
#define SENTINEL   0x7FFF0000u     /* the return address of every call the host makes */
#define RESULT     (TEB + 0x800u)  /* a double the x87 stubs load */

static uc_engine *g_uc;
static uint8_t *g_img;
static uint8_t g_gdt[0x1000];
static uint32_t g_heap = HEAP;
static int g_stop; static uint32_t g_stop_at;
static int g_fault_ok, g_faulted;             /* vm mode: a fault, a watched access or the limit ends the call */
static uint32_t g_fault_at;
static unsigned long g_insn_limit = 200000000ul;
enum { STOP_NONE, STOP_RET, STOP_HOST, STOP_FAULT, STOP_EVENT };
/* vm mode: why the call ended (g_faulted), see run_vm */
enum { EV_NONE, EV_FAULT, EV_FRAME, EV_INPUTS, EV_NOISE, EV_SCAN, EV_LIMIT, EV_TEMP };
static int g_event; static uint32_t g_event_eip;

static void die(const char *fmt, ...){
    va_list a; va_start(a, fmt);
    fputs("fxoracle: ", stderr); vfprintf(stderr, fmt, a); fputc('\n', stderr);
    va_end(a); exit(1);
}
static uint32_t rd32(uint32_t a){ uint32_t v = 0; if (uc_mem_read(g_uc, a, &v, 4)) die("read 0x%08x", a); return v; }
static void wr32(uint32_t a, uint32_t v){ if (uc_mem_write(g_uc, a, &v, 4)) die("write 0x%08x", a); }
static void rdmem(uint32_t a, void *p, uint32_t n){ if (n && uc_mem_read(g_uc, a, p, n)) die("read 0x%08x+%u", a, n); }
static void wrmem(uint32_t a, const void *p, uint32_t n){ if (n && uc_mem_write(g_uc, a, p, n)) die("write 0x%08x+%u", a, n); }
static uint32_t reg(int r){ uint32_t v = 0; uc_reg_read(g_uc, r, &v); return v; }
static void setreg(int r, uint32_t v){ uc_reg_write(g_uc, r, &v); }

static uint32_t gmalloc(uint32_t n){
    uint32_t a = (g_heap + 15u) & ~15u;
    if (n > HEAP + HEAPSZ - a - 16u) die("guest heap full");
    g_heap = a + 16u + ((n + 15u) & ~15u);
    wr32(a, n);                                    /* the block's size, for realloc */
    return a + 16u;
}
static uint32_t gcalloc(uint32_t n){
    uint32_t a = gmalloc(n);
    static const uint8_t z[4096] = { 0 };
    for (uint32_t i = 0; i < n; i += sizeof z) wrmem(a + i, z, n - i < sizeof z ? n - i : (uint32_t)sizeof z);
    return a;
}

/* ---- host functions: the imports and the fake objects' methods ---- */
typedef int (*host_fn)(uint32_t esp, uint32_t *eax);   /* returns the argument bytes the callee pops */
typedef struct { char name[64]; host_fn fn; uint32_t arg; } hostent;
static hostent g_host[1024];
static uint32_t g_nhost;
static uint32_t host_add(const char *name, host_fn fn, uint32_t arg){
    if (g_nhost == sizeof g_host / sizeof g_host[0]) die("too many host functions");
    snprintf(g_host[g_nhost].name, sizeof g_host[0].name, "%s", name);
    g_host[g_nhost].fn = fn; g_host[g_nhost].arg = arg;
    return HOSTCALL + 16u * g_nhost++;
}
static uint32_t g_cur_arg;          /* the running host function's arg */

static bool on_unmapped(uc_engine *uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void *ud){
    (void)uc; (void)size; (void)value; (void)ud;
    uint32_t a = (uint32_t)addr;
    if (type == UC_MEM_FETCH_UNMAPPED && a == SENTINEL){ g_stop = STOP_RET; return false; }
    if (type == UC_MEM_FETCH_UNMAPPED && a >= HOSTCALL && a < HOSTCALL + 16u * g_nhost && !(a & 15u)){
        g_stop = STOP_HOST; g_stop_at = a; return false;
    }
    g_stop = STOP_FAULT; g_stop_at = a;
    return false;
}

/* Run from eip until the code returns to SENTINEL (the caller planted it). */
static void run_until_return(uint32_t eip){
    for (;;){
        g_stop = STOP_NONE;
        uc_err e = uc_emu_start(g_uc, eip, 0, 0, g_insn_limit);
        if (g_stop == STOP_RET) return;
        if (g_stop == STOP_HOST){
            uint32_t k = (g_stop_at - HOSTCALL) / 16u, esp = reg(UC_X86_REG_ESP), eax = reg(UC_X86_REG_EAX);
            uint32_t ret = rd32(esp);
            g_cur_arg = g_host[k].arg;
            int pop = g_host[k].fn(esp, &eax);
            if (pop < 0) die("import %s is not implemented (called from 0x%08x)", g_host[k].name, ret);
            setreg(UC_X86_REG_EAX, eax);
            setreg(UC_X86_REG_ESP, esp + 4u + (uint32_t)pop);
            eip = ret;
            continue;
        }
        if (g_stop == STOP_FAULT && g_fault_ok){
            g_faulted = 1; g_fault_at = g_stop_at; g_event = EV_FAULT; g_event_eip = reg(UC_X86_REG_EIP); return;
        }
        if (g_stop == STOP_EVENT && g_fault_ok){ g_faulted = 1; return; }      /* a hook set g_event */
        if (g_stop == STOP_NONE && !e && g_fault_ok){ g_faulted = 1; g_event = EV_LIMIT; return; }
        if (g_stop == STOP_FAULT) die("fault at 0x%08x, eip 0x%08x", g_stop_at, reg(UC_X86_REG_EIP));
        die("stopped at eip 0x%08x: %s", reg(UC_X86_REG_EIP), e ? uc_strerror(e) : "instruction limit");
    }
}

/* A stdcall or cdecl call from the host: the arguments, then SENTINEL. */
static uint32_t g_esp_top = STK + STKSZ - 0x1000u;
static uint32_t gcall(uint32_t fn, int n, ...){
    uint32_t args[16];
    va_list a; va_start(a, n);
    for (int i = 0; i < n; i++) args[i] = va_arg(a, uint32_t);
    va_end(a);
    uint32_t esp0 = reg(UC_X86_REG_ESP), saved_top = g_esp_top;
    uint32_t esp = (esp0 > STK && esp0 <= g_esp_top ? esp0 : g_esp_top) - 64u;
    esp = (esp & ~15u) - 4u * (uint32_t)n - 4u;
    wr32(esp, SENTINEL);
    for (int i = 0; i < n; i++) wr32(esp + 4u + 4u * (uint32_t)i, args[i]);
    g_esp_top = esp;
    setreg(UC_X86_REG_ESP, esp);
    run_until_return(fn);
    g_esp_top = saved_top;
    uint32_t eax = reg(UC_X86_REG_EAX);
    setreg(UC_X86_REG_ESP, esp0);
    return eax;
}

/* ---- set-up ---- */
static void gdt_entry(int i, uint32_t base, uint32_t limit, int code, int dpl){
    uint8_t *d = g_gdt + 8 * i;
    limit >>= 12;
    d[0] = limit & 0xFF; d[1] = (limit >> 8) & 0xFF;
    d[2] = base & 0xFF; d[3] = (base >> 8) & 0xFF; d[4] = (base >> 16) & 0xFF;
    d[5] = (uint8_t)(0x80 | (dpl << 5) | 0x10 | (code ? 0x0A : 0x02));
    d[6] = (uint8_t)(0xC0 | ((limit >> 16) & 0x0F));
    d[7] = (base >> 24) & 0xFF;
}
static void vm_open(void){
    if (uc_open(UC_ARCH_X86, UC_MODE_32, &g_uc)) die("uc_open");
    uc_hook h;
    uc_hook_add(g_uc, &h, UC_HOOK_MEM_UNMAPPED, (void *)on_unmapped, NULL, 1, 0);
    if (uc_mem_map_ptr(g_uc, DLL_BASE, DLL_SIZE, UC_PROT_ALL, g_img) ||
        uc_mem_map(g_uc, HEAP, HEAPSZ, UC_PROT_READ | UC_PROT_WRITE) ||
        uc_mem_map(g_uc, SCR, SCRSZ, UC_PROT_READ | UC_PROT_WRITE) ||
        uc_mem_map(g_uc, STK, STKSZ, UC_PROT_READ | UC_PROT_WRITE) ||
        uc_mem_map(g_uc, TEB, 0x1000, UC_PROT_READ | UC_PROT_WRITE) ||
        uc_mem_map(g_uc, STUBS, 0x10000, UC_PROT_ALL) ||
        uc_mem_map_ptr(g_uc, GDT_VA, 0x1000, UC_PROT_READ | UC_PROT_WRITE, g_gdt)) die("uc_mem_map");
    gdt_entry(1, 0, 0xFFFFF000u, 1, 3);
    gdt_entry(2, 0, 0xFFFFF000u, 0, 3);
    gdt_entry(3, 0, 0xFFFFF000u, 0, 0);
    gdt_entry(4, TEB, 0x00000FFFu, 0, 3);
    uc_x86_mmr gdtr = { 0, GDT_VA, 8 * 5 - 1, 0 };
    uc_reg_write(g_uc, UC_X86_REG_GDTR, &gdtr);
    uint32_t sel = 3 << 3; uc_reg_write(g_uc, UC_X86_REG_SS, &sel);
    sel = (2 << 3) | 3; uc_reg_write(g_uc, UC_X86_REG_DS, &sel); uc_reg_write(g_uc, UC_X86_REG_ES, &sel);
    uc_reg_write(g_uc, UC_X86_REG_GS, &sel);
    sel = (4 << 3) | 3; uc_reg_write(g_uc, UC_X86_REG_FS, &sel);
    wr32(TEB + 0x00, 0xFFFFFFFFu);                  /* no SEH frame */
    wr32(TEB + 0x04, STK + STKSZ); wr32(TEB + 0x08, STK);
    wr32(TEB + 0x18, TEB);
    uint16_t cw = 0x027F;                           /* Windows' default: 53-bit precision, nearest, all masked */
    uc_reg_write(g_uc, UC_X86_REG_FPCW, &cw);
    setreg(UC_X86_REG_ESP, g_esp_top);
}

/* ---- the imports ---- */
static double rdd(uint32_t a){ double v; rdmem(a, &v, 8); return v; }
static void wrd(uint32_t a, double v){ wrmem(a, &v, 8); }
static uint32_t gstrlen(uint32_t a){ uint32_t n = 0; uint8_t ch; do { rdmem(a + n, &ch, 1); n++; } while (ch); return n - 1; }
static int g_verbose;

#define ARG(i) rd32(esp + 4u + 4u * (i))
static int h_zero(uint32_t esp, uint32_t *eax){ (void)esp; *eax = 0; return (int)g_cur_arg; }
static int h_one(uint32_t esp, uint32_t *eax){ (void)esp; *eax = 1; return (int)g_cur_arg; }
static int h_missing(uint32_t esp, uint32_t *eax){ (void)esp; (void)eax; return -1; }
static int h_malloc(uint32_t esp, uint32_t *eax){ *eax = gmalloc(ARG(0)); return 0; }
static int h_calloc(uint32_t esp, uint32_t *eax){
    uint64_t n = (uint64_t)ARG(0) * ARG(1);
    *eax = n > HEAPSZ ? 0 : gcalloc((uint32_t)n); return 0;
}
static int h_free(uint32_t esp, uint32_t *eax){ (void)esp; (void)eax; return 0; }
static int h_realloc(uint32_t esp, uint32_t *eax){
    uint32_t p = ARG(0), n = ARG(1);
    uint32_t q = gmalloc(n);
    if (p){
        uint32_t old = rd32(p - 16u), m = old < n ? old : n;
        uint8_t *t = malloc(m ? m : 1); rdmem(p, t, m); wrmem(q, t, m); free(t);
    }
    *eax = q; return 0;
}
static int h_heapalloc(uint32_t esp, uint32_t *eax){
    *eax = ARG(1) & 8u ? gcalloc(ARG(2)) : gmalloc(ARG(2)); return 12;
}
static int h_memmove(uint32_t esp, uint32_t *eax){
    uint32_t d = ARG(0), s = ARG(1), n = ARG(2);
    uint8_t *t = malloc(n ? n : 1); rdmem(s, t, n); wrmem(d, t, n); free(t);
    *eax = d; return 0;
}
static int h_interlocked(uint32_t esp, uint32_t *eax){       /* arg: 0 inc, 1 dec, 2 exchange, 3 compare-exchange */
    uint32_t p = ARG(0), v = rd32(p);
    switch (g_cur_arg){
    case 0: wr32(p, v + 1); *eax = v + 1; return 4;
    case 1: wr32(p, v - 1); *eax = v - 1; return 4;
    case 2: wr32(p, ARG(1)); *eax = v; return 8;
    default: if (v == ARG(2)) wr32(p, ARG(1)); *eax = v; return 12;
    }
}
static int h_versionex(uint32_t esp, uint32_t *eax){
    uint32_t p = ARG(0);
    wr32(p + 4, 5); wr32(p + 8, 1); wr32(p + 12, 2600); wr32(p + 16, 2);   /* Windows XP */
    *eax = 1; return 4;
}
static int h_sysinfo(uint32_t esp, uint32_t *eax){
    uint32_t p = ARG(0);
    static const uint8_t z[36] = { 0 }; wrmem(p, z, 36);
    wr32(p + 4, 0x1000); wr32(p + 8, 0x10000); wr32(p + 12, 0x7FFEFFFF); wr32(p + 16, 1); wr32(p + 20, 1);
    wr32(p + 24, 586); wr32(p + 28, 0x10000); *eax = 0; return 4;
}
static int h_debugstr(uint32_t esp, uint32_t *eax){
    if (g_verbose){
        uint32_t a = ARG(0), n = gstrlen(a); char *s = calloc(1, n + 1); rdmem(a, s, n);
        fprintf(stderr, "fxoracle: dll: %s", s); free(s);
    }
    *eax = 0; return 4;
}
static int h_qpc(uint32_t esp, uint32_t *eax){ wr32(ARG(0), 0x12345678u); wr32(ARG(0) + 4, 0); *eax = 1; return 4; }
static int h_filetime(uint32_t esp, uint32_t *eax){ wr32(ARG(0), 0x89ABCDEFu); wr32(ARG(0) + 4, 0x01C00000u); *eax = 0; return 4; }
static int h_virtualalloc(uint32_t esp, uint32_t *eax){ *eax = gcalloc(ARG(1)); return 16; }
static int h_memstatus(uint32_t esp, uint32_t *eax){
    uint32_t p = ARG(0);
    wr32(p, 32); for (uint32_t i = 4; i < 32; i += 4) wr32(p + i, 0x10000000u);
    *eax = 0; return 4;
}
static int g_features;                                         /* --features: SSE and SSE2 present */
static int h_feature(uint32_t esp, uint32_t *eax){
    uint32_t f = ARG(0);
    *eax = g_features && (f == 6 || f == 10);                   /* PF_XMMI_INSTRUCTIONS_AVAILABLE, PF_XMMI64_... */
    return 4;
}
static int h_regerr(uint32_t esp, uint32_t *eax){ (void)esp; *eax = 2; return (int)g_cur_arg; }   /* ERROR_FILE_NOT_FOUND */
static int h_stricmp(uint32_t esp, uint32_t *eax){
    uint32_t a = ARG(0), b = ARG(1);
    for (;; a++, b++){
        uint8_t x, y; rdmem(a, &x, 1); rdmem(b, &y, 1);
        int cx = x >= 'A' && x <= 'Z' ? x + 32 : x, cy = y >= 'A' && y <= 'Z' ? y + 32 : y;
        if (cx != cy || !x){ *eax = (uint32_t)(cx - cy); return 0; }
    }
}
static int h_strchr(uint32_t esp, uint32_t *eax){
    uint32_t a = ARG(0); uint8_t c = (uint8_t)ARG(1);
    for (;; a++){ uint8_t x; rdmem(a, &x, 1); if (x == c){ *eax = a; return 0; } if (!x){ *eax = 0; return 0; } }
}
static int h_wcslen(uint32_t esp, uint32_t *eax){
    uint32_t a = ARG(0), n = 0; uint16_t x;
    for (;; n++){ rdmem(a + 2 * n, &x, 2); if (!x) break; }
    *eax = n; return 0;
}
static int h_ctype(uint32_t esp, uint32_t *eax){
    int c = (int)ARG(0);
    switch (g_cur_arg){
    case 0: *eax = c >= 'a' && c <= 'z' ? (uint32_t)(c - 32) : (uint32_t)c; break;         /* toupper */
    case 1: *eax = c >= 'A' && c <= 'Z' ? (uint32_t)(c + 32) : (uint32_t)c; break;         /* tolower */
    case 2: *eax = c == ' ' || (c >= 9 && c <= 13); break;                                  /* isspace */
    case 3: *eax = c >= '0' && c <= '9'; break;                                             /* isdigit */
    case 4: *eax = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); break;                 /* isalpha */
    case 5: *eax = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); break;
    default: *eax = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); break;
    }
    return 0;
}
static int h_atoi(uint32_t esp, uint32_t *eax){
    uint32_t a = ARG(0), n = gstrlen(a); char *s = calloc(1, n + 1); rdmem(a, s, n);
    *eax = (uint32_t)atol(s); free(s); return 0;
}
/* _controlfp(new, mask): msvcrt's abstract control word against the x87's */
static uint32_t cw_to_abstract(uint16_t cw){
    uint32_t r = 0;
    if (cw & 0x01) r |= 0x10; if (cw & 0x02) r |= 0x80000; if (cw & 0x04) r |= 0x08;   /* invalid, denormal, zero divide */
    if (cw & 0x08) r |= 0x04; if (cw & 0x10) r |= 0x02; if (cw & 0x20) r |= 0x01;       /* overflow, underflow, inexact */
    r |= (uint32_t)((cw >> 10) & 3) << 8;                                                /* RC: same order */
    switch ((cw >> 8) & 3){ case 0: r |= 0x20000; break; case 2: r |= 0x10000; break; default: break; }   /* PC 24, 53, 64 */
    if (cw & 0x1000) r |= 0x40000;
    return r;
}
static uint16_t abstract_to_cw(uint32_t a, uint16_t cw){
    cw &= (uint16_t)~0x1F3Fu;
    if (a & 0x10) cw |= 0x01; if (a & 0x80000) cw |= 0x02; if (a & 0x08) cw |= 0x04;
    if (a & 0x04) cw |= 0x08; if (a & 0x02) cw |= 0x10; if (a & 0x01) cw |= 0x20;
    cw |= (uint16_t)(((a >> 8) & 3) << 10);
    cw |= (uint16_t)((a & 0x20000) ? 0 << 8 : (a & 0x10000) ? 2 << 8 : 3 << 8);
    if (a & 0x40000) cw |= 0x1000;
    return cw;
}
static unsigned long g_controlfp_calls;
static int h_controlfp(uint32_t esp, uint32_t *eax){
    uint16_t cw = 0; uc_reg_read(g_uc, UC_X86_REG_FPCW, &cw);
    uint32_t nw = ARG(0), mask = ARG(1) & 0x000F031Fu & ~0u;   /* EM, RC, PC (IC is not kept by later CPUs) */
    mask = ARG(1) & (0x0008001Fu | 0x300u | 0x30000u | 0x40000u);
    uint32_t cur = cw_to_abstract(cw), nx = (cur & ~mask) | (nw & mask);
    cw = abstract_to_cw(nx, cw);
    uc_reg_write(g_uc, UC_X86_REG_FPCW, &cw);
    g_controlfp_calls++;
    *eax = cw_to_abstract(cw); return 0;
}
static uint32_t g_rand = 1;
static int h_srand(uint32_t esp, uint32_t *eax){ g_rand = ARG(0); *eax = 0; return 0; }
static int h_rand(uint32_t esp, uint32_t *eax){ (void)esp; g_rand = g_rand * 214013u + 2531011u; *eax = g_rand >> 16 & 0x7FFF; return 0; }
static int h_finite(uint32_t esp, uint32_t *eax){ *eax = isfinite(rdd(esp + 4)) != 0; return 0; }
static int h_isnan(uint32_t esp, uint32_t *eax){ *eax = isnan(rdd(esp + 4)) != 0; return 0; }
/* x87 helpers, reached through the guest stubs (the operands at esp + 4 ...) */
static int h_x87(uint32_t esp, uint32_t *eax){
    double r = 0;
    switch (g_cur_arg){
    case 0: r = floor(rdd(esp + 8)); break;                 /* floor: cdecl, after two return addresses */
    case 1: r = ceil(rdd(esp + 8)); break;
    case 2: r = pow(rdd(esp + 12), rdd(esp + 4)); break;     /* _CIpow: st1 ^ st0 */
    case 3: r = fmod(rdd(esp + 12), rdd(esp + 4)); break;    /* _CIfmod: st1 mod st0 */
    case 4: r = asin(rdd(esp + 4)); break;
    case 5: r = acos(rdd(esp + 4)); break;
    case 6: r = sqrt(rdd(esp + 4)); break;
    case 7: r = exp(rdd(esp + 4)); break;
    case 8: r = cosh(rdd(esp + 4)); break;
    case 9: r = sinh(rdd(esp + 4)); break;
    default: r = tanh(rdd(esp + 4)); break;
    }
    wrd(RESULT, r); *eax = 0; return 0;
}
static uint32_t g_stub_at = STUBS;
static uint32_t stub_code(const uint8_t *c, uint32_t n){
    uint32_t a = g_stub_at; wrmem(a, c, n); g_stub_at = (a + n + 15u) & ~15u; return a;
}
/* call HOSTCALL k at the next 5 bytes of p (at guest a) */
static uint32_t put_call(uint8_t *p, uint32_t a, uint32_t target){
    int32_t rel = (int32_t)(target - (a + 5u)); p[0] = 0xE8; memcpy(p + 1, &rel, 4); return 5;
}
/* x87 stub: kind 0 cdecl double -> st0, 1 st0 -> st0, 2 st1, st0 -> st0 */
static uint32_t x87_stub(const char *name, int kind, uint32_t which){
    uint32_t h = host_add(name, h_x87, which), a = g_stub_at;
    uint8_t c[64]; uint32_t n = 0;
    if (kind == 1){ const uint8_t s[] = { 0x83, 0xEC, 0x08, 0xDD, 0x1C, 0x24 }; memcpy(c, s, sizeof s); n = sizeof s; }
    if (kind == 2){ const uint8_t s[] = { 0x83, 0xEC, 0x10, 0xDD, 0x1C, 0x24, 0xDD, 0x5C, 0x24, 0x08 }; memcpy(c, s, sizeof s); n = sizeof s; }
    n += put_call(c + n, a + n, h);
    if (kind == 1){ c[n++] = 0x83; c[n++] = 0xC4; c[n++] = 0x08; }
    if (kind == 2){ c[n++] = 0x83; c[n++] = 0xC4; c[n++] = 0x10; }
    c[n++] = 0xDD; c[n++] = 0x05; uint32_t r = RESULT; memcpy(c + n, &r, 4); n += 4;     /* fld qword [RESULT] */
    c[n++] = 0xC3;
    return stub_code(c, n);
}
static uint32_t initterm_stub(void){
    static const uint8_t c[] = { 0x56, 0x57, 0x8B, 0x74, 0x24, 0x0C, 0x8B, 0x7C, 0x24, 0x10, 0x3B, 0xF7, 0x73, 0x0D,
                                 0x8B, 0x06, 0x85, 0xC0, 0x74, 0x02, 0xFF, 0xD0, 0x83, 0xC6, 0x04, 0xEB, 0xEF,
                                 0x5F, 0x5E, 0xC3 };
    return stub_code(c, sizeof c);
}

/* qsort(base, n, size, cmp): an insertion sort that calls the guest's comparator (the host runs between
 * emulations here, so it can start another one); stable, where msvcrt's is not: D3DX sorts CTAB entries by
 * register set and index (0x4F97C4), keys that do not tie in a valid table */
static uint32_t gcall(uint32_t fn, int n, ...);
static int h_qsort(uint32_t esp, uint32_t *eax){
    uint32_t base = ARG(0), n = ARG(1), sz = ARG(2), cmp = ARG(3);
    uint8_t *key = malloc(sz ? sz : 1), *t = malloc(sz ? sz : 1);
    uint32_t tmp = gmalloc(sz ? sz : 1);
    for (uint32_t i = 1; i < n; i++){
        rdmem(base + i * sz, key, sz); wrmem(tmp, key, sz);
        uint32_t j = i;
        while (j > 0 && (int32_t)gcall(cmp, 2, base + (j - 1) * sz, tmp) > 0){
            rdmem(base + (j - 1) * sz, t, sz); wrmem(base + j * sz, t, sz); j--;
        }
        wrmem(base + j * sz, key, sz);
    }
    free(key); free(t);
    *eax = 0; return 0;
}

typedef struct { const char *name; host_fn fn; uint32_t arg; } impdef;
static const impdef k_imports[] = {
    /* msvcrt.dll (cdecl) */
    { "malloc", h_malloc, 0 }, { "calloc", h_calloc, 0 }, { "realloc", h_realloc, 0 }, { "free", h_free, 0 },
    { "??2@YAPAXI@Z", h_malloc, 0 }, { "??3@YAXPAX@Z", h_free, 0 }, { "memmove", h_memmove, 0 },
    { "_controlfp", h_controlfp, 0 }, { "_stricmp", h_stricmp, 0 }, { "strchr", h_strchr, 0 },
    { "wcslen", h_wcslen, 0 }, { "toupper", h_ctype, 0 }, { "tolower", h_ctype, 1 }, { "isspace", h_ctype, 2 },
    { "isdigit", h_ctype, 3 }, { "isalpha", h_ctype, 4 }, { "isalnum", h_ctype, 5 }, { "isxdigit", h_ctype, 6 },
    { "atoi", h_atoi, 0 }, { "atol", h_atoi, 0 }, { "srand", h_srand, 0 }, { "rand", h_rand, 0 },
    { "_finite", h_finite, 0 }, { "_isnan", h_isnan, 0 }, { "qsort", h_qsort, 0 },
    /* KERNEL32.dll (stdcall: arg = bytes popped where the handler takes it) */
    { "GetProcessHeap", h_one, 0 }, { "HeapAlloc", h_heapalloc, 0 }, { "HeapFree", h_one, 12 },
    { "InitializeCriticalSection", h_zero, 4 }, { "EnterCriticalSection", h_zero, 4 },
    { "LeaveCriticalSection", h_zero, 4 }, { "DeleteCriticalSection", h_zero, 4 },
    { "InterlockedIncrement", h_interlocked, 0 }, { "InterlockedDecrement", h_interlocked, 1 },
    { "InterlockedExchange", h_interlocked, 2 }, { "InterlockedCompareExchange", h_interlocked, 3 },
    { "GetCurrentThreadId", h_one, 0 }, { "GetCurrentProcessId", h_one, 0 }, { "GetTickCount", h_one, 0 },
    { "QueryPerformanceCounter", h_qpc, 0 }, { "GetSystemTimeAsFileTime", h_filetime, 0 },
    { "DisableThreadLibraryCalls", h_one, 4 }, { "GetVersionExA", h_versionex, 0 },
    { "IsProcessorFeaturePresent", h_feature, 0 }, { "GetSystemInfo", h_sysinfo, 0 },
    { "GetModuleHandleA", h_zero, 4 }, { "GetProcAddress", h_zero, 8 }, { "LoadLibraryA", h_zero, 4 },
    { "FreeLibrary", h_one, 4 }, { "VirtualAlloc", h_virtualalloc, 0 }, { "VirtualFree", h_one, 12 },
    { "OutputDebugStringA", h_debugstr, 0 }, { "CreateMutexA", h_one, 12 }, { "WaitForSingleObject", h_zero, 8 },
    { "ReleaseMutex", h_one, 4 }, { "CloseHandle", h_one, 4 }, { "GetLastError", h_zero, 0 },
    { "SetUnhandledExceptionFilter", h_zero, 4 }, { "GetCurrentProcess", h_one, 0 },
    { "GlobalMemoryStatus", h_memstatus, 0 }, { "GetACP", h_one, 0 }, { "IsDBCSLeadByte", h_zero, 4 },
    /* ADVAPI32.dll */
    { "RegOpenKeyA", h_regerr, 12 }, { "RegQueryValueExA", h_regerr, 24 }, { "RegCloseKey", h_zero, 4 },
};

/* ---- load the DLL ---- */
static uint32_t g_nexp; static struct { char name[64]; uint32_t va; } *g_exp;
static uint32_t dll_export(const char *name){
    for (uint32_t i = 0; i < g_nexp; i++) if (!strcmp(g_exp[i].name, name)) return g_exp[i].va;
    die("no export %s", name);
    return 0;
}
static uint8_t *slurp(const char *path, uint32_t *size){
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 ? malloc((size_t)n) : NULL;
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n){ fclose(f); free(b); return NULL; }
    fclose(f); *size = (uint32_t)n; return b;
}
static int read_conf(const char *path, const char *key, char *out, size_t n){
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512]; size_t kl = strlen(key); int ok = 0;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, kl) && line[kl] == '='){
            snprintf(out, n, "%s", line + kl + 1); out[strcspn(out, "\r\n")] = 0; ok = 1;
        }
    fclose(f); return ok;
}
static void dll_load(const char *path, const char *conf){
    uint32_t n; uint8_t *d = slurp(path, &n);
    if (!d) die("cannot read %s", path);
    char got[65], want[80];
    sha256_hex(d, n, got);
    if (!read_conf(conf, "D3DX_SHA256", want, sizeof want)) die("%s has no D3DX_SHA256", conf);
    if (strcmp(got, want)) die("%s: SHA-256 %s is not game.conf's D3DX_SHA256", path, got);
    uint32_t nt = fx2_rd32(d + 0x3C), oh = nt + 24, opt = fx2_rd16(d + nt + 20), nsec = fx2_rd16(d + nt + 6);
    if (fx2_rd32(d + oh + 28) != DLL_BASE || fx2_rd32(d + oh + 56) != DLL_SIZE) die("unexpected image base or size");
    g_img = calloc(1, DLL_SIZE);
    memcpy(g_img, d, fx2_rd32(d + oh + 60));
    for (uint32_t i = 0; i < nsec; i++){
        const uint8_t *s = d + oh + opt + 40 * i;
        uint32_t vs = fx2_rd32(s + 8), va = fx2_rd32(s + 12), raw = fx2_rd32(s + 16), ptr = fx2_rd32(s + 20);
        if (raw > vs) raw = vs;
        if (va + raw > DLL_SIZE || ptr + raw > n) die("section %u outside", i);
        memcpy(g_img + va, d + ptr, raw);
    }
    free(d);
    vm_open();
    /* the imports */
    uint32_t irva = fx2_rd32(g_img + oh + 96 + 8);
    uint32_t zero_word = TEB + 0x900u;
    for (uint32_t di = irva; ; di += 20){
        uint32_t oft = fx2_rd32(g_img + di), name = fx2_rd32(g_img + di + 12), ft = fx2_rd32(g_img + di + 16);
        if (!name && !ft) break;
        for (uint32_t k = 0; ; k += 4){
            uint32_t v = fx2_rd32(g_img + (oft ? oft : ft) + k);
            if (!v) break;
            const char *nm = (const char *)g_img + v + 2;
            uint32_t tgt = 0;
            if (!strcmp(nm, "_adjust_fdiv")) tgt = zero_word;              /* a variable: 0, no FDIV bug */
            else if (!strcmp(nm, "_initterm")) tgt = initterm_stub();
            else if (!strcmp(nm, "floor")) tgt = x87_stub(nm, 0, 0);
            else if (!strcmp(nm, "ceil")) tgt = x87_stub(nm, 0, 1);
            else if (!strcmp(nm, "_CIpow")) tgt = x87_stub(nm, 2, 2);
            else if (!strcmp(nm, "_CIfmod")) tgt = x87_stub(nm, 2, 3);
            else if (!strcmp(nm, "_CIasin")) tgt = x87_stub(nm, 1, 4);
            else if (!strcmp(nm, "_CIacos")) tgt = x87_stub(nm, 1, 5);
            else if (!strcmp(nm, "_CIsqrt")) tgt = x87_stub(nm, 1, 6);
            else if (!strcmp(nm, "_CIexp")) tgt = x87_stub(nm, 1, 7);
            else if (!strcmp(nm, "_CIcosh")) tgt = x87_stub(nm, 1, 8);
            else if (!strcmp(nm, "_CIsinh")) tgt = x87_stub(nm, 1, 9);
            else if (!strcmp(nm, "_CItanh")) tgt = x87_stub(nm, 1, 10);
            else {
                const impdef *def = NULL;
                for (size_t j = 0; j < sizeof k_imports / sizeof k_imports[0] && !def; j++)
                    if (!strcmp(k_imports[j].name, nm)) def = &k_imports[j];
                tgt = def ? host_add(nm, def->fn, def->arg) : host_add(nm, h_missing, 0);
            }
            memcpy(g_img + ft + k, &tgt, 4);
        }
    }
    /* the exports */
    uint32_t erva = fx2_rd32(g_img + oh + 96);
    uint32_t nn = fx2_rd32(g_img + erva + 24), af = fx2_rd32(g_img + erva + 28), an = fx2_rd32(g_img + erva + 32),
             ao = fx2_rd32(g_img + erva + 36);
    g_exp = calloc(nn, sizeof *g_exp); g_nexp = nn;
    for (uint32_t k = 0; k < nn; k++){
        snprintf(g_exp[k].name, sizeof g_exp[k].name, "%s", (const char *)g_img + fx2_rd32(g_img + an + 4 * k));
        g_exp[k].va = DLL_BASE + fx2_rd32(g_img + af + 4u * fx2_rd16(g_img + ao + 2 * k));
    }
    /* DllMain(hinst, DLL_PROCESS_ATTACH, NULL) */
    uint32_t entry = DLL_BASE + fx2_rd32(g_img + oh + 16);
    if (!gcall(entry, 3, DLL_BASE, 1u, 0u)) die("DllMain failed");
}

/* ---- the fake IDirect3DDevice9 ---- */
#define NDEV (sizeof M_DEVICE / sizeof M_DEVICE[0])
typedef struct { uint32_t slot, n, a[8], nd, d[26]; } devcall;   /* d: the struct a call points to */
static devcall *g_calls; static uint32_t g_ncalls, g_capcalls;
static uint32_t g_dev, g_obj;                  /* the device; a generic object for anything it creates */
static uint32_t g_shader_vt;                   /* IDirect3D{Vertex,Pixel}Shader9: +4 the code, +8 its bytes */
static int h_dev(uint32_t esp, uint32_t *eax){
    uint32_t slot = g_cur_arg, nargs = M_DEVICE[slot].nargs;
    if (g_ncalls == g_capcalls){ g_capcalls = g_capcalls ? 2 * g_capcalls : 4096; g_calls = realloc(g_calls, g_capcalls * sizeof *g_calls); }
    devcall *c = &g_calls[g_ncalls++];
    c->slot = slot; c->n = nargs - 1; c->nd = 0;
    for (uint32_t i = 0; i + 1 < nargs && i < 8; i++) c->a[i] = ARG(i + 1);
    {
        const char *nm = M_DEVICE[slot].name;
        uint32_t ptr = 0, cnt = 0;
        if (!strcmp(nm, "SetTransform")){ ptr = ARG(2); cnt = 16; }
        else if (!strcmp(nm, "SetMaterial")){ ptr = ARG(1); cnt = 17; }
        else if (!strcmp(nm, "SetLight")){ ptr = ARG(2); cnt = 26; }
        if (ptr){ rdmem(ptr, c->d, 4 * cnt); c->nd = cnt; }
    }
    *eax = 0;
    const char *nm = M_DEVICE[slot].name;
    if (!strcmp(nm, "AddRef") || !strcmp(nm, "Release")) *eax = 1;
    else if (!strcmp(nm, "QueryInterface")) *eax = 0x80004002u;            /* E_NOINTERFACE */
    else if (!strcmp(nm, "CreateVertexShader") || !strcmp(nm, "CreatePixelShader")){
        /* a shader object that gives its code back (GetFunction): copied, with its size from the token walk */
        uint32_t fn = ARG(1), ndw = 0;
        for (uint32_t cap = 4096; !ndw && cap <= 0x40000; cap *= 2){
            uint8_t *t = malloc(cap);
            if (uc_mem_read(g_uc, fn, t, cap)){ free(t); break; }
            fx2_shader s; fx2_shader_walk(t, cap, &s, NULL, NULL);
            if (s.ended || (s.stop < s.ndw && fx2_rd32(t + 4 * s.stop) == FX2_END)) ndw = s.stop + 1;
            free(t);
        }
        if (!ndw) die("a shader without an end token at 0x%08x", fn);
        uint32_t o = gcalloc(16), code = gmalloc(4 * ndw);
        uint8_t *t = malloc(4 * ndw); rdmem(fn, t, 4 * ndw); wrmem(code, t, 4 * ndw); free(t);
        wr32(o, g_shader_vt); wr32(o + 4, code); wr32(o + 8, 4 * ndw);
        wr32(ARG(2), o);
    }
    else if (!strcmp(nm, "CreateVertexDeclaration") || !strcmp(nm, "CreateStateBlock") || !strcmp(nm, "CreateQuery"))
        wr32(ARG(2), g_obj);
    else if (!strcmp(nm, "EndStateBlock")) wr32(ARG(1), g_obj);
    else if (!strcmp(nm, "GetDeviceCaps")){
        uint32_t p = ARG(1);
        uint8_t z[304] = { 0 }; wrmem(p, z, sizeof z);
        wr32(p + 196, 0xFFFE0300u); wr32(p + 204, 0xFFFF0300u);             /* VertexShaderVersion, PixelShaderVersion */
        wr32(p + 200, 256); wr32(p + 208, 0x7F7FFFFF);                        /* MaxVertexShaderConst, PixelShader1xMaxValue */
        wr32(p + 116, 8); wr32(p + 120, 8);                                   /* MaxTextureBlendStages, MaxSimultaneousTextures */
    }
    else if (!strcmp(nm, "GetDirect3D") || !strcmp(nm, "GetVertexShader") || !strcmp(nm, "GetPixelShader") ||
             !strcmp(nm, "GetTexture") || !strcmp(nm, "GetVertexDeclaration") || !strcmp(nm, "GetIndices"))
        wr32(ARG(nargs - 2), 0);
    return (int)(4u * nargs);
}
static int h_shader(uint32_t esp, uint32_t *eax){
    uint32_t o = ARG(0);
    *eax = 0;
    switch (g_cur_arg){
    case 0: *eax = 0x80004002u; return 12;                         /* QueryInterface */
    case 1: case 2: *eax = 1; return 4;                            /* AddRef, Release */
    case 3: wr32(ARG(1), g_dev); return 8;                         /* GetDevice */
    default: {                                                     /* GetFunction(data, &size) */
        uint32_t code = rd32(o + 4), n = rd32(o + 8), data = ARG(1), psz = ARG(2);
        if (data){
            if (rd32(psz) < n){ *eax = 0x8876086Cu; return 12; }
            uint8_t *t = malloc(n); rdmem(code, t, n); wrmem(data, t, n); free(t);
        }
        wr32(psz, n);
        return 12;
    }
    }
}
/* every other object the device makes (vertex declarations, queries, state blocks): QueryInterface, AddRef,
 * Release, GetDevice, and IDirect3DStateBlock9's Capture and Apply as no-ops */
static int h_obj(uint32_t esp, uint32_t *eax){
    *eax = g_cur_arg == 1 || g_cur_arg == 2 ? 1 : 0;
    if (g_cur_arg == 0){ *eax = 0x80004002u; return 12; }
    if (g_cur_arg == 3){ wr32(ARG(1), g_dev); return 8; }
    return 4;
}
static void make_device(void){
    uint32_t vt = gcalloc(4 * (uint32_t)NDEV);
    for (uint32_t i = 0; i < NDEV; i++){
        char nm[64]; snprintf(nm, sizeof nm, "IDirect3DDevice9::%s", M_DEVICE[i].name);
        wr32(vt + 4 * i, host_add(nm, h_dev, i));
    }
    g_dev = gcalloc(16); wr32(g_dev, vt);
    uint32_t ovt = gcalloc(4 * 16);
    for (uint32_t i = 0; i < 16; i++){
        char nm[64]; snprintf(nm, sizeof nm, "object slot %u", i);
        wr32(ovt + 4 * i, host_add(nm, i < 6 ? h_obj : h_missing, i));
    }
    g_obj = gcalloc(16); wr32(g_obj, ovt);
    g_shader_vt = gcalloc(4 * 5);
    for (uint32_t i = 0; i < 5; i++){
        static const char *const nm[5] = { "QueryInterface", "AddRef", "Release", "GetDevice", "GetFunction" };
        char full[64]; snprintf(full, sizeof full, "shader::%s", nm[i]);
        wr32(g_shader_vt + 4 * i, host_add(full, h_shader, i));
    }
}
static uint32_t vcall(uint32_t obj, uint32_t slot, int n, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3){
    uint32_t fn = rd32(rd32(obj) + 4 * slot);
    switch (n){
    case 0: return gcall(fn, 1, obj);
    case 1: return gcall(fn, 2, obj, a0);
    case 2: return gcall(fn, 3, obj, a0, a1);
    case 3: return gcall(fn, 4, obj, a0, a1, a2);
    default: return gcall(fn, 5, obj, a0, a1, a2, a3);
    }
}

/* ---- a whitespace tokenizer (as tools/mw/fxpresrun.c's) ---- */
typedef struct { char *p; const char *path; unsigned line; int nl; } lex;
static char *tok(lex *L){
    if (L->nl){ L->nl = 0; L->line++; return "\n"; }
    while (*L->p == ' ' || *L->p == '\t' || *L->p == '\r') L->p++;
    if (!*L->p) return NULL;
    if (*L->p == '\n'){ L->p++; L->line++; return "\n"; }
    char *t = L->p;
    while (*L->p && *L->p != ' ' && *L->p != '\t' && *L->p != '\r' && *L->p != '\n') L->p++;
    if (*L->p == '\n') L->nl = 1;
    if (*L->p) *L->p++ = 0;
    return t;
}
static char *need(lex *L){ char *t = tok(L); if (!t || !strcmp(t, "\n")) die("%s:%u: missing field", L->path, L->line); return t; }
static void eol(lex *L){ char *t = tok(L); if (t && strcmp(t, "\n")) die("%s:%u: extra field %s", L->path, L->line, t); }
static uint64_t num(lex *L, const char *t, int hex){
    char *e; unsigned long long v = strtoull(t, &e, hex ? 16 : 10);
    if (*e || !*t) die("%s:%u: not a number: %s", L->path, L->line, t);
    return v;
}
static void expect(lex *L, const char *want){
    char *t = need(L);
    if (strcmp(t, want)) die("%s:%u: %s where %s was expected", L->path, L->line, t, want);
}
static char *read_text(const char *path){
    uint32_t n; uint8_t *b = slurp(path, &n);
    if (!b) die("cannot read %s", path);
    b = realloc(b, (size_t)n + 1); b[n] = 0;
    return (char *)b;
}

/* ---- vm: the interpreter in isolation on "fxpcase 1" ---- */
/* The loader's check runs on the blob at V_BLOB with zeros after it.  The interpreter then runs on a copy of the
 * blob and of each table placed so that each ends where an unmapped page begins (G_*), with its 256 input slots
 * filled with an unmapped address (POISON) and its temporaries at 0, and it is stopped at the first thing it does
 * with memory that is not the case's (fx-preshader §6.2):
 *   fault <addr>        an access to an unmapped address: past a table, the literals or the blob (the page after
 *                       it), through table 9 or a missing v (address 0), or through a slot no input set (POISON)
 *   frame <addr> <eip>  a data access (8 bytes, or mov's rep movsd) to its own frame or above: the slot array and
 *                       its locals, where temporaries past 255 lie
 *   temp <addr> <eip>   with "temps unknown": a read of a temporary no instruction of the run wrote (D3DX reads
 *                       what its stack held, fx-preshader §5.1), from the writes and reads of the 256 doubles
 *   inputs              an input's slot written past the 256 (the 257th input)
 *   noise               a call of the noise function (opcodes 0x502, 0x711)
 *   limit               the instruction limit
 * and the loader's check is stopped as "load scan" at the first read past the blob's end by the comment search
 * (D3DXFindShaderComment) or of the version or a count (0x4E9158, 0x4E91BE, 0x4E91C9): the reads its other checks
 * make past the end always end in E_FAIL (fx-preshader §2.2).  Facts from the DLL's code (X), fx-preshader §1. */
#define VA_PRESCHECK 0x004E9146u  /* the loader's check (stdcall: blob, size, &fxlc, &clit), fx-preshader §2.2 */
#define VA_FINDCMT   0x004DA2E5u  /* D3DXFindShaderComment, up to VA_FINDCMT_END */
#define VA_FINDCMT_END 0x004DA3CCu
#define VA_CHK_VER   0x004E9158u  /* the check's reads of the version, the CLIT count and the FXLC count */
#define VA_CHK_CLITN 0x004E91BEu
#define VA_CHK_FXLCN 0x004E91C9u
#define VA_SLOTSTORE 0x004E89A8u  /* mov [ebp + esi*4 - 0x41C], eax: an input's slot */
#define VA_MOVSD     0x004E8D5Fu  /* mov (0x100): rep movsd */
#define VA_NOISE     0x0044318Fu  /* the noise function the two noise opcodes call */
#define FRAME_SLOTS  0x41Cu       /* ebp - 0x41C: the 256 slots; ebp - 0xC1C: the 256 temporaries */
#define FRAME_TEMPS  0xC1Cu
#define POISON       0x41414141u
#define V_BLOB  (SCR + 0x00000u)
#define V_TMAX  0x8000u           /* doubles per table */
#define G_BASE  0x21000000u       /* the run's copies: blob, c, o, v, each region followed by unmapped space */
#define G_STEP  0x00100000u
#define G_SIZE  0x00040000u
static int g_phase;                                   /* 1 the loader's check, 2 the interpreter */
static uint32_t g_blob_end, g_frame_lo;
static int g_temps_track; static uint32_t g_temps_lo;  /* "temps unknown": the temporaries' dwords written so far */
static uint8_t g_twrote[0x800 / 4];
static void vm_event(uc_engine *uc, int ev, uint32_t addr){
    g_event = ev; g_stop_at = addr; g_fault_at = addr; g_event_eip = reg(UC_X86_REG_EIP);
    g_stop = STOP_EVENT; uc_emu_stop(uc);
}
static bool on_vm_mem(uc_engine *uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void *ud){
    (void)value; (void)ud;
    if (g_stop == STOP_EVENT) return true;
    uint32_t a = (uint32_t)addr, eip = reg(UC_X86_REG_EIP);
    if (g_phase == 1){
        if (a < SCR + SCRSZ && a + (uint32_t)size > g_blob_end && ((eip >= VA_FINDCMT && eip < VA_FINDCMT_END) || eip == VA_CHK_VER ||
                                                eip == VA_CHK_CLITN || eip == VA_CHK_FXLCN))
            vm_event(uc, EV_SCAN, a);
    } else if (g_phase == 2 && a + (uint32_t)size > g_frame_lo){
        if (eip == VA_SLOTSTORE){ if (a >= g_frame_lo + 0x400u) vm_event(uc, EV_INPUTS, a); }
        else if (size == 8 || eip == VA_MOVSD) vm_event(uc, EV_FRAME, a);
    } else if (g_phase == 2 && g_temps_track && a < g_temps_lo + 0x800u && a + (uint32_t)size > g_temps_lo){
        uint32_t lo = a > g_temps_lo ? a : g_temps_lo, hi = a + (uint32_t)size < g_temps_lo + 0x800u ? a + (uint32_t)size
                                                                                                   : g_temps_lo + 0x800u;
        for (uint32_t d = (lo - g_temps_lo) / 4; d < (hi - g_temps_lo + 3) / 4; d++){
            if (type == UC_MEM_WRITE) g_twrote[d] = 1;
            else if (!g_twrote[d]){ vm_event(uc, EV_TEMP, a); break; }
        }
    }
    return true;
}
static void on_vm_noise(uc_engine *uc, uint64_t addr, uint32_t size, void *ud){
    (void)size; (void)ud;
    if (g_phase == 2) vm_event(uc, EV_NOISE, (uint32_t)addr);
}
static void fpu_reset(uint16_t cw){              /* an empty x87 stack, no flags, control word cw */
    uint16_t sw = 0, tag = 0xFFFF;
    uc_reg_write(g_uc, UC_X86_REG_FPSW, &sw); uc_reg_write(g_uc, UC_X86_REG_FPTAG, &tag);
    uc_reg_write(g_uc, UC_X86_REG_FPCW, &cw);
}
static uint16_t pc_cw(int pc){ return (uint16_t)(pc == 24 ? 0x007F : pc == 53 ? 0x027F : 0x037F); }
static uint64_t *read_doubles(lex *L, uint32_t *n, uint32_t *mask){
    *n = (uint32_t)num(L, need(L), 0);
    expect(L, "mask");
    *mask = (uint32_t)num(L, need(L), 0);
    if (*n > V_TMAX) die("%s:%u: a table of %u doubles (at most %u here)", L->path, L->line, *n, V_TMAX);
    uint64_t *t = calloc(*n ? *n : 1, 8);
    for (uint32_t i = 0; i < *n; i++) t[i] = num(L, need(L), 1);
    eol(L);
    return t;
}
/* table k's copy for the run: its n doubles end where region k ends */
static uint32_t put_guarded(int k, const void *data, uint32_t bytes){
    uint32_t a = G_BASE + (uint32_t)k * G_STEP + G_SIZE - bytes;
    wrmem(a, data, bytes);
    return a;
}
static int run_vm(const char *in, const char *outp){
    char *text = read_text(in);
    lex L = { text, in, 1, 0 };
    FILE *f = strcmp(outp, "-") ? fopen(outp, "w") : stdout;
    if (!f) die("cannot write %s", outp);
    expect(&L, "fxpcase"); expect(&L, "1"); eol(&L);
    fprintf(f, "fxpres 1 oracle\n");
    for (uint32_t k = 0; k < 4; k++)
        if (uc_mem_map(g_uc, G_BASE + k * G_STEP, G_SIZE, UC_PROT_READ | UC_PROT_WRITE)) die("uc_mem_map");
    uc_hook hm, hn;
    uc_hook_add(g_uc, &hm, UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, (void *)on_vm_mem, NULL, SCR, SCR + SCRSZ - 1);
    uc_hook hs;
    uc_hook_add(g_uc, &hs, UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, (void *)on_vm_mem, NULL, STK, STK + STKSZ - 1);
    uc_hook_add(g_uc, &hn, UC_HOOK_CODE, (void *)on_vm_noise, NULL, VA_NOISE, VA_NOISE);
    static uint8_t zero[0x40000];
    static uint32_t poison[256];
    for (int i = 0; i < 256; i++) poison[i] = POISON;
    uint8_t *blob = malloc(0x40000);
    unsigned long ncase = 0, nstop = 0;
    for (;;){
        char *t = tok(&L);
        while (t && !strcmp(t, "\n")) t = tok(&L);
        if (!t) break;
        if (strcmp(t, "case")) die("%s:%u: %s where case was expected", in, L.line, t);
        char name[256]; snprintf(name, sizeof name, "%s", need(&L)); eol(&L);
        wrmem(V_BLOB, zero, sizeof zero);
        int pc = 64, run = 1, havev = 0, unknown = 0; uint32_t nb = 0, nc = 0, cm = 0, no = 0, om = 0, nv = 0, vm = 0;
        uint64_t *c = NULL, *o = NULL, *v = NULL;
        for (;;){
            t = need(&L);
            if (!strcmp(t, "end")){ eol(&L); break; }
            if (!strcmp(t, "pc")){ pc = (int)num(&L, need(&L), 0); eol(&L); }
            else if (!strcmp(t, "run")){ run = (int)num(&L, need(&L), 0); eol(&L); }
            else if (!strcmp(t, "temps")){ expect(&L, "unknown"); eol(&L); unknown = 1; }
            else if (!strcmp(t, "blob")){
                nb = (uint32_t)num(&L, need(&L), 0);
                if (nb > 0x3F000) die("%s:%u: a blob of %u bytes", in, L.line, nb);
                for (uint32_t i = 0; i < nb; i += 4){ uint32_t w = (uint32_t)num(&L, need(&L), 1); wrmem(V_BLOB + i, &w, 4); }
                eol(&L);
            }
            else if (!strcmp(t, "c")){ free(c); c = read_doubles(&L, &nc, &cm); }
            else if (!strcmp(t, "o")){ free(o); o = read_doubles(&L, &no, &om); }
            else if (!strcmp(t, "v")){ free(v); v = read_doubles(&L, &nv, &vm); havev = 1; }
            else die("%s:%u: unknown line %s", in, L.line, t);
        }
        ncase++;
        uint32_t pf = gcalloc(8), fxlc_p = pf, clit_p = pf + 4;
        setreg(UC_X86_REG_ESP, g_esp_top);
        fpu_reset(0x027F);
        g_fault_ok = 1; g_faulted = 0; g_event = EV_NONE; g_phase = 1; g_blob_end = V_BLOB + nb;
        uint32_t hr = gcall(VA_PRESCHECK, 4, V_BLOB, nb, fxlc_p, clit_p);
        g_fault_ok = 0; g_phase = 0;
        if (g_faulted && g_event == EV_FAULT && g_fault_at >= g_blob_end && g_event_eip >= VA_FINDCMT &&
            g_event_eip < VA_FINDCMT_END) g_event = EV_SCAN;            /* a skip past the scratch region */
        if (g_faulted){
            if (g_event == EV_SCAN) fprintf(f, "case %s load scan 0x%08x\n", name, g_event_eip);
            else fprintf(f, "case %s load fault 0x%08x\n", name, g_fault_at);
            nstop++; free(c); free(o); free(v); continue;
        }
        fprintf(f, "case %s load %s\n", name, (int32_t)hr < 0 ? "fail" : "ok");
        if ((int32_t)hr < 0 || !run){ free(c); free(o); free(v); continue; }
        /* the run's copies (the check may have made a CLIT count of 0 a 1 in the blob: copied after it) */
        rdmem(V_BLOB, blob, nb);
        uint32_t gb = put_guarded(0, blob, nb);
        uint32_t fxlc = rd32(fxlc_p) - V_BLOB + gb, clit = rd32(clit_p) - V_BLOB + gb;
        uint32_t gc = put_guarded(1, c, 8 * nc), go = put_guarded(2, o, 8 * no), gv = havev ? put_guarded(3, v, 8 * nv) : 0;
        /* the interpreter, on a stack cleared below the call (its temporaries start at 0) with its slots poisoned */
        fpu_reset(pc_cw(pc));
        uint32_t esp = g_esp_top - 64;
        for (uint32_t a = esp - 0x10000; a < esp; a += sizeof zero) wrmem(a, zero, esp - a < sizeof zero ? esp - a : (uint32_t)sizeof zero);
        setreg(UC_X86_REG_ESP, esp + 64u);
        uint32_t ret_slot = (((esp & ~15u) - 4u * 14u - 4u));             /* where gcall puts the return address */
        uint32_t ebp = ret_slot - 4u;
        wrmem(ebp - FRAME_SLOTS, poison, sizeof poison);
        g_temps_track = unknown; g_temps_lo = ebp - FRAME_TEMPS; memset(g_twrote, 0, sizeof g_twrote);
        g_fault_ok = 1; g_faulted = 0; g_event = EV_NONE; g_phase = 2; g_frame_lo = ebp - FRAME_SLOTS;
        gcall(VA_FXLCVM, 14, fxlc, clit, gv, gc, 0u, go, go, go, havev ? vm : 0u, cm, 0u, om, om, om);
        g_fault_ok = 0; g_phase = 0; g_temps_track = 0;
        if (g_faulted){
            switch (g_event){
            case EV_FRAME: fprintf(f, "  run frame 0x%08x 0x%08x\n", g_fault_at, g_event_eip); break;
            case EV_TEMP: fprintf(f, "  run temp 0x%08x 0x%08x\n", g_fault_at, g_event_eip); break;
            case EV_INPUTS: fprintf(f, "  run inputs\n"); break;
            case EV_NOISE: fprintf(f, "  run noise\n"); break;
            case EV_LIMIT: fprintf(f, "  run limit\n"); break;
            default: fprintf(f, "  run fault 0x%08x\n", g_fault_at); break;
            }
            nstop++; free(c); free(o); free(v); continue;
        }
        fprintf(f, "  run ok\n  o");
        for (uint32_t i = 0; i < no; i++){ uint64_t x; rdmem(go + 8 * i, &x, 8); fprintf(f, " %016llx", (unsigned long long)x); }
        fprintf(f, "\n  r");
        int any = 0;
        uint32_t temps = ebp - FRAME_TEMPS;
        for (uint32_t i = 0; i < 256; i++){
            uint64_t x; rdmem(temps + 8 * i, &x, 8);
            if (x){ fprintf(f, " %u=%016llx", i, (unsigned long long)x); any = 1; }
        }
        fprintf(f, "%s\n", any ? "" : " -");
        free(c); free(o); free(v);
    }
    fprintf(f, "end\n");
    if (f != stdout) fclose(f);
    free(text); free(blob);
    uc_hook_del(g_uc, hm); uc_hook_del(g_uc, hs); uc_hook_del(g_uc, hn);
    fprintf(stderr, "fxoracle vm: %lu cases, %lu stopped\n", ncase, nstop);
    return 0;
}

/* ---- effect: D3DX's own evaluation, through the device it calls (fx-preshader §6.3) ---- */
#define FX_GETPARAMETERBYNAME 9u                     /* ID3DXEffect slots (docs/mw/fx-model.md §1.1) */
#define FX_GETTECHNIQUE 12u
#define FX_SETVALUE 20u
#define FX_SETTECHNIQUE 58u
#define FX_BEGIN 63u
#define FX_BEGINPASS 64u
#define FX_ENDPASS 66u
#define FX_END 67u
#define D3DXFX_DONOTSAVESTATE 1u
/* the typed setters a "call" line names (docs/mw/fx-model.md §1.1): kind 0 a value, 1 a pointer, 2 a pointer and
 * a count */
typedef struct { const char *name; uint32_t slot; int kind; } setterdef;
static const setterdef k_setters[] = {
    { "SetBool", 22, 0 }, { "SetBoolArray", 24, 2 }, { "SetInt", 26, 0 }, { "SetIntArray", 28, 2 },
    { "SetFloat", 30, 0 }, { "SetFloatArray", 32, 2 }, { "SetVector", 34, 1 }, { "SetVectorArray", 36, 2 },
    { "SetMatrix", 38, 1 }, { "SetMatrixArray", 40, 2 }, { "SetMatrixTranspose", 44, 1 },
    { "SetMatrixTransposeArray", 46, 2 } };
static const setterdef *setter_get(const char *n){
    for (size_t i = 0; i < sizeof k_setters / sizeof k_setters[0]; i++) if (!strcmp(k_setters[i].name, n)) return &k_setters[i];
    return NULL;
}
static uint16_t g_cws[8]; static int g_ncw;          /* the control words the interpreter ran at, in a pass */
static uint32_t g_begin_flags = D3DXFX_DONOTSAVESTATE; /* --begin-flags: Begin's flags (MW passes 0 at 10 sites) */
static void on_vm_cw(uc_engine *uc, uint64_t addr, uint32_t size, void *ud){
    (void)uc; (void)addr; (void)size; (void)ud;
    uint16_t cw; uc_reg_read(g_uc, UC_X86_REG_FPCW, &cw);
    for (int i = 0; i < g_ncw; i++) if (g_cws[i] == cw) return;
    if (g_ncw < 8) g_cws[g_ncw++] = cw;
}
typedef struct { char *name; uint32_t effect, ntech; } oeff;
static int by_name(const void *a, const void *b){ return strcmp(((const oeff *)a)->name, ((const oeff *)b)->name); }
static void dump_calls(FILE *f){
    for (uint32_t i = 0; i < g_ncalls; i++){
        const devcall *c = &g_calls[i];
        const char *nm = M_DEVICE[c->slot].name;
        if (!strcmp(nm, "SetRenderState")) fprintf(f, "    rs %u %08x\n", c->a[0], c->a[1]);
        else if (!strcmp(nm, "SetTextureStageState")) fprintf(f, "    tss %u %u %08x\n", c->a[0], c->a[1], c->a[2]);
        else if (!strcmp(nm, "SetSamplerState")) fprintf(f, "    ss %u %u %08x\n", c->a[0], c->a[1], c->a[2]);
        else if (!strcmp(nm, "LightEnable")) fprintf(f, "    lightenable %u %08x\n", c->a[0], c->a[1]);
        else if (c->nd){
            if (!strcmp(nm, "SetTransform")) fprintf(f, "    transform %u", c->a[0]);
            else if (!strcmp(nm, "SetMaterial")) fprintf(f, "    material");
            else fprintf(f, "    light %u", c->a[0]);
            for (uint32_t k = 0; k < c->nd; k++) fprintf(f, " %08x", c->d[k]);
            fputc('\n', f);
        }
    }
}
static int run_effect(const char *in, const char *outp, int nfiles, char **files){
    make_device();
    uc_hook hk; uc_hook_add(g_uc, &hk, UC_HOOK_CODE, (void *)on_vm_cw, NULL, VA_FXLCVM, VA_FXLCVM);
    oeff *E = calloc((size_t)nfiles, sizeof *E);
    for (int i = 0; i < nfiles; i++){
        const char *b = strrchr(files[i], '/'); b = b ? b + 1 : files[i];
        size_t l = strlen(b); if (l > 4 && !strcmp(b + l - 4, ".fxo")) l -= 4;
        E[i].name = calloc(1, l + 1); memcpy(E[i].name, b, l);
        E[i].effect = (uint32_t)i;                          /* the file, until it is created */
    }
    char **byname = calloc((size_t)nfiles, sizeof *byname);
    for (int i = 0; i < nfiles; i++) byname[i] = files[i];
    /* created in byte order of NAME with one pool, as A and B make them */
    for (int i = 0; i < nfiles; i++) for (int k = i + 1; k < nfiles; k++)
        if (strcmp(E[k].name, E[i].name) < 0){ oeff t = E[i]; E[i] = E[k]; E[k] = t; char *u = byname[i]; byname[i] = byname[k]; byname[k] = u; }
    (void)by_name;
    uint32_t pp = gcalloc(4);
    if (gcall(dll_export("D3DXCreateEffectPool"), 1, pp)) die("D3DXCreateEffectPool failed");
    for (int i = 0; i < nfiles; i++){
        uint32_t n; uint8_t *b = slurp(byname[i], &n);
        if (!b) die("cannot read %s", byname[i]);
        uint32_t src = gmalloc(n); wrmem(src, b, n); free(b);
        uint32_t pe = gcalloc(4), perr = gcalloc(4);
        uint32_t hr = gcall(dll_export("D3DXCreateEffect"), 9, g_dev, src, n, 0u, 0u, 0u, rd32(pp), pe, perr);
        if (hr) die("%s: D3DXCreateEffect 0x%08x", E[i].name, hr);
        E[i].effect = rd32(pe);
        uint32_t desc = gcalloc(16);
        vcall(E[i].effect, 3, 1, desc, 0, 0, 0);
        E[i].ntech = rd32(desc + 8);
    }
    char *text = read_text(in);
    lex L = { text, in, 1, 0 };
    FILE *f = strcmp(outp, "-") ? fopen(outp, "w") : stdout;
    if (!f) die("cannot write %s", outp);
    expect(&L, "fxpround"); expect(&L, "1"); eol(&L);
    fprintf(f, "fxpdev 1\n");
    int pc = 24, next_pc = 24; unsigned long nround = 0, nset = 0, ncall = 0;
    char round[64] = "";
    uint32_t buf = gcalloc(4096), name = gcalloc(1024), pn = gcalloc(4);
    for (;;){
        char *t = tok(&L);
        while (t && !strcmp(t, "\n")) t = tok(&L);
        if (!t) break;
        if (!strcmp(t, "set")){
            char *en = need(&L), *path = need(&L);
            uint32_t n = (uint32_t)num(&L, need(&L), 0);
            if (n > 1024) die("%s:%u: %u dwords", in, L.line, n);
            for (uint32_t k = 0; k < n; k++) wr32(buf + 4 * k, (uint32_t)num(&L, need(&L), 1));
            eol(&L);
            oeff *e = NULL;
            for (int i = 0; i < nfiles && !e; i++) if (!strcmp(E[i].name, en)) e = &E[i];
            if (!e) die("%s:%u: no effect %s", in, L.line, en);
            size_t pl = strlen(path); if (pl > 1000) die("a long name");
            wrmem(name, path, (uint32_t)pl + 1);
            fpu_reset(0x027F);
            uint32_t h = vcall(e->effect, FX_GETPARAMETERBYNAME, 2, 0, name, 0, 0);
            if (!h) die("%s:%u: %s has no parameter %s", in, L.line, en, path);
            uint32_t hr = vcall(e->effect, FX_SETVALUE, 3, h, buf, 4 * n, 0);
            if (hr) die("%s:%u: SetValue %s.%s: 0x%08x", in, L.line, en, path, hr);
            nset++;
            continue;
        }
        if (!strcmp(t, "call")){                    /* a typed setter through the vtable (fx-preshader §6.3) */
            char *setter = need(&L), *en = need(&L), *path = need(&L);
            uint32_t count = (uint32_t)num(&L, need(&L), 0), n = (uint32_t)num(&L, need(&L), 0);
            if (n > 1024) die("%s:%u: %u dwords", in, L.line, n);
            for (uint32_t k = 0; k < n; k++) wr32(buf + 4 * k, (uint32_t)num(&L, need(&L), 1));
            eol(&L);
            const setterdef *sd = setter_get(setter);
            if (!sd) die("%s:%u: no setter %s", in, L.line, setter);
            oeff *e = NULL;
            for (int i = 0; i < nfiles && !e; i++) if (!strcmp(E[i].name, en)) e = &E[i];
            if (!e) die("%s:%u: no effect %s", in, L.line, en);
            size_t pl = strlen(path); if (pl > 1000) die("a long name");
            wrmem(name, path, (uint32_t)pl + 1);
            fpu_reset(0x027F);
            uint32_t h = vcall(e->effect, FX_GETPARAMETERBYNAME, 2, 0, name, 0, 0);
            if (!h) die("%s:%u: %s has no parameter %s", in, L.line, en, path);
            fpu_reset(pc_cw(pc));                       /* the round's precision: SetVector's colour form rounds at it */
            uint32_t hr = sd->kind == 0 ? vcall(e->effect, sd->slot, 2, h, n ? rd32(buf) : 0, 0, 0)
                        : sd->kind == 1 ? vcall(e->effect, sd->slot, 2, h, buf, 0, 0)
                        : vcall(e->effect, sd->slot, 3, h, buf, count, 0);
            fprintf(f, "call %s %s %s = %08x\n", setter, en, path, hr);
            ncall++;
            continue;
        }
        if (!strcmp(t, "round") || !strcmp(t, "end")){
            int last = !strcmp(t, "end");
            char next[64] = "";
            if (!last){ snprintf(next, sizeof next, "%s", need(&L)); expect(&L, "pc"); next_pc = (int)num(&L, need(&L), 0); }
            eol(&L);
            if (round[0]){
                fprintf(f, "round %s pc %d\n", round, pc);
                for (int i = 0; i < nfiles; i++){
                    fprintf(f, "effect %s\n", E[i].name);
                    for (uint32_t ti = 0; ti < E[i].ntech; ti++){
                        fpu_reset(0x027F);
                        uint32_t th = vcall(E[i].effect, FX_GETTECHNIQUE, 1, ti, 0, 0, 0);
                        if (vcall(E[i].effect, FX_SETTECHNIQUE, 1, th, 0, 0, 0)) die("%s: SetTechnique %u", E[i].name, ti);
                        g_ncw = 0;
                        if (vcall(E[i].effect, FX_BEGIN, 2, pn, g_begin_flags, 0, 0)) die("%s: Begin", E[i].name);
                        if (g_ncw){
                            fprintf(f, "  tech %u begin cw", ti);
                            for (int k = 0; k < g_ncw; k++) fprintf(f, " %04x", g_cws[k]);
                            fputc('\n', f);
                        }
                        for (uint32_t pi = 0; pi < rd32(pn); pi++){
                            g_ncalls = 0; g_ncw = 0;
                            fpu_reset(pc_cw(pc));                   /* the game thread's precision (§3.4) */
                            uint32_t hr = vcall(E[i].effect, FX_BEGINPASS, 1, pi, 0, 0, 0);
                            fprintf(f, "  tech %u pass %u", ti, pi);
                            if (hr) fprintf(f, " BeginPass %08x", hr);
                            if (g_ncw){ fprintf(f, " cw"); for (int k = 0; k < g_ncw; k++) fprintf(f, " %04x", g_cws[k]); }
                            fputc('\n', f);
                            dump_calls(f);
                            vcall(E[i].effect, FX_ENDPASS, 0, 0, 0, 0, 0);
                        }
                        vcall(E[i].effect, FX_END, 0, 0, 0, 0, 0);
                    }
                }
                nround++;
            }
            snprintf(round, sizeof round, "%s", next);
            pc = next_pc;
            if (last) break;
            continue;
        }
        die("%s:%u: unknown line %s", in, L.line, t);
    }
    fprintf(f, "end\n");
    if (f != stdout) fclose(f);
    free(text);
    fprintf(stderr, "fxoracle effect: %d effects, %lu rounds, %lu sets, %lu calls\n", nfiles, nround, nset, ncall);
    return 0;
}

int main(int argc, char **argv){
    const char *dll = getenv("NFSMW_D3DX"), *conf = "games/mw/game.conf";
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] == '-'; i++){
        if (!strcmp(argv[i], "--dll") && i + 1 < argc) dll = argv[++i];
        else if (!strcmp(argv[i], "--conf") && i + 1 < argc) conf = argv[++i];
        else if (!strcmp(argv[i], "--verbose")) g_verbose = 1;
        else if (!strcmp(argv[i], "--features")) g_features = 1;
        else if (!strcmp(argv[i], "--begin-flags") && i + 1 < argc) g_begin_flags = (uint32_t)strtoul(argv[++i], NULL, 0);
        else die("unknown option %s", argv[i]);
    }
    if (!dll || !*dll) die("no DLL: --dll <d3dx9_26.dll> or NFSMW_D3DX");
    dll_load(dll, conf);
    if (g_verbose) fprintf(stderr, "fxoracle: DllMain ok, %u host functions\n", g_nhost);
    if (i + 2 < argc && !strcmp(argv[i], "vm")) return run_vm(argv[i + 1], argv[i + 2]);
    if (i + 3 < argc && !strcmp(argv[i], "effect")) return run_effect(argv[i + 1], argv[i + 2], argc - i - 3, argv + i + 3);
    fprintf(stderr, "usage: fxoracle [--dll <d3dx9_26.dll>] [--conf <game.conf>] [--verbose] [--features] vm <cases.txt> <out.txt|->\n"
                    "       fxoracle [...] [--begin-flags <n>] effect <rounds.txt> <out.txt|-> <NAME>.fxo...\n");
    return 2;
}
