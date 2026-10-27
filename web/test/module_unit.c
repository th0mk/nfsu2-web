/* runtime/module.c and runtime/modheap.c with the game's lifted DLLs
 * (web/test/module-wasm.test.mjs):
 *   WASI_DIR=/t=<empty folder> module_unit.wasm /t
 * With MW's map (-DNFSU2_GAME_MW) the game lists two DLLs, server.dll and
 * d3dx9_26.dll, each in its slot and with its own heaps (memmap_mw.h); with
 * UG2's, server.dll alone with two heaps (memmap_ug2.h).  The test writes
 * small DLL files into the folder (a .text, an IAT, an export, a HIGHLOW
 * relocation), pins each ModInfo to its file's SHA-256 and loads them the way
 * LoadLibraryA does.  Each DLL's lifted code is a host function here: its
 * DllMain calls HeapCreate through its own IAT slot as the DLL's C runtime
 * does, routed as win32_call routes it (modheap_create of the module the
 * import index belongs to), and takes blocks from that heap.  Prints a line
 * per check and "module_unit: <n> passed, <m> failed". */
#include "cpu.h"
#include "game.h"
#include "memmap.h"
#include "module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMP_MAGIC 0xF0000000u            /* module.c, win32.c */
void nfsu2_sha256(const void *data, size_t n, uint8_t out[32]);   /* runtime/web/sha256.c */
void mem_commit(uint32_t addr, uint32_t size);
int modheap_owns(uint32_t h); uint32_t modheap_create(int mod); int modheap_destroy(uint32_t h);
uint32_t modheap_alloc(uint32_t h, uint32_t n, uint32_t flags); int modheap_free(uint32_t h, uint32_t u);
void modheap_report(void);

static int pass, fail;
#define CHECK(cond, ...) do { if (cond) pass++; else { fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *g_dir;

/* ---- the exe's imports: two, before the modules' in the index space ---- */
const ImpEnt g_imp[] = { {0x00401000u, "KERNEL32.dll", "GetTickCount", 0}, {0x00401004u, "KERNEL32.dll", "Sleep", 1} };
const uint32_t g_imp_count = 2;

/* ---- the DLLs: layout of each file and image ---- */
#define IMG_SIZE  0x4000u
#define RVA_MAIN  0x1010u                /* DllMain */
#define RVA_WORK  0x1020u                /* the export "Work" */
#define RVA_FIXED 0x1100u                /* a dword relocated by the loader: pref + RVA_DATA */
#define RVA_DATA  0x2000u                /* the IAT: HeapCreate, HeapAlloc, HeapDestroy */
#define RVA_EXP   0x2100u
#define RVA_REL   0x3000u
#define PREF      0x10000000u

#ifdef NFSU2_GAME_MW
#define NMOD 2
static const char *const g_names[NMOD] = { "server.dll", "d3dx9_26.dll" };
static const uint32_t g_bases[NMOD] = { MM_SRV_LO, MM_D3DX_LO };
#else
#define NMOD 1
static const char *const g_names[NMOD] = { "server.dll" };
static const uint32_t g_bases[NMOD] = { MM_SRV_LO };
#endif

static char g_sha[NMOD][65];
static ModInfo g_info[NMOD];
static ImpEnt g_mimp[NMOD][3];
static const uint32_t g_mimp_count[NMOD] = { 3
#if NMOD > 1
    , 3
#endif
};
static uint32_t g_fn_addr[NMOD][2];
static mod_fn g_fn_ptr[NMOD][2];
static const uint32_t g_fn_count[NMOD] = { 2
#if NMOD > 1
    , 2
#endif
};
static ModLink g_links[NMOD];           /* filled before module_init */
const GameOps g_game = { .id = MM_GAME, .exe_name = "GAME.EXE", .vroot = "C:\\GAME", .module_path = "C:\\GAME\\GAME.EXE",
                         .modules = g_links, .nmodules = NMOD };

/* ---- the lifted code: host functions ---- */
static uint32_t g_heap[NMOD], g_block[NMOD][4], g_main_calls[NMOD][2];
static uint32_t arg(CPU *c, int i){ return MR32(c->esp + 4u + 4u * (uint32_t)i); }
/* win32_call's routing of a module's HeapCreate (win32.c) */
static uint32_t import_heapcreate(uint32_t iat_slot, int *mod_out){
    uint32_t idx = MR32(iat_slot) - IMP_MAGIC;
    int mod = idx >= g_imp_count ? module_of_import(idx) : 0;
    if (mod_out) *mod_out = mod;
    return mod ? modheap_create(mod) : 0;
}
static void dll_main(CPU *c, int k){
    uint32_t base = arg(c, 0), reason = arg(c, 1);
    g_main_calls[k][reason ? 1 : 0]++;
    c->eax = 1;
    if (reason == 1){                    /* PROCESS_ATTACH: the C runtime makes its heap */
        int mod = -1;
        g_heap[k] = import_heapcreate(base + RVA_DATA, &mod);
        CHECK(mod == k + 1, "%s's HeapCreate import belongs to module %d (wanted %d)", g_names[k], mod, k + 1);
        if (!g_heap[k]){ c->eax = 0; return; }
        static const uint32_t sizes[4] = { 24, 4096, 5000, 300000 };
        for (int i = 0; i < 4; i++) g_block[k][i] = modheap_alloc(g_heap[k], sizes[i], i == 0 ? 8u : 0u);
    } else if (reason == 0){             /* PROCESS_DETACH */
        if (g_heap[k]) modheap_destroy(g_heap[k]);
        g_heap[k] = 0;
    }
}
static void work(CPU *c, int k){ c->eax = 100u * (uint32_t)(k + 1) + arg(c, 0); }
static void main0(CPU *c){ dll_main(c, 0); }
static void work0(CPU *c){ work(c, 0); }
#if NMOD > 1
static void main1(CPU *c){ dll_main(c, 1); }
static void work1(CPU *c){ work(c, 1); }
#endif

/* ---- what module.c needs of the rest of the runtime ---- */
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args){
    CPU save = *c;
    for (int i = nargs - 1; i >= 0; i--) PUSH32(args[i]);
    PUSH32(0xDEADBEEFu);
    mod_fn f = module_fn_at(fn);
    if (!f){ printf("FAIL call_guest: no lifted function at 0x%08x\n", fn); fail++; *c = save; return 0; }
    f(c);
    uint32_t r = c->eax;
    *c = save;
    return r;
}
int fileio_resolve_game(const char *win, char *out, size_t outsz){
    snprintf(out, outsz, "%s/%s", g_dir, win);
    FILE *f = fopen(out, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}
uint64_t win32_ext_threads(void){ return 0; }
uint64_t win32_ext_threads_exited(void){ return 0; }
uint32_t win32_heap_live(void){ return 0; }
int win32_tls_in_use(void){ return 0; }
int win32_obj_live(void){ return 0; }
uint32_t com_scratch(uint32_t n){ (void)n; return 0; }
void guest_sleep_ns(int64_t ns){ (void)ns; }

/* ---- a DLL file ---- */
static void w32(uint8_t *p, uint32_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void w16(uint8_t *p, uint32_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void make_dll(int k){
    static uint8_t f[IMG_SIZE];
    memset(f, 0, sizeof f);
    f[0] = 'M'; f[1] = 'Z'; w32(f + 0x3c, 0x80);
    uint8_t *nt = f + 0x80, *opt = nt + 24;
    memcpy(nt, "PE\0\0", 4); w16(nt + 4, 0x14c); w16(nt + 6, 3); w16(nt + 20, 0xe0); w16(nt + 22, 0x2102);
    w16(opt, 0x10b); w32(opt + 16, RVA_MAIN); w32(opt + 28, PREF); w32(opt + 32, 0x1000); w32(opt + 36, 0x1000);
    w32(opt + 56, IMG_SIZE); w32(opt + 60, 0x1000); w32(opt + 92, 16);
    w32(opt + 96, RVA_EXP); w32(opt + 100, 0x100);              /* exports */
    w32(opt + 96 + 5 * 8, RVA_REL); w32(opt + 96 + 5 * 8 + 4, 12);   /* base relocations */
    static const char *const sn[3] = { ".text", ".data", ".reloc" };
    for (int i = 0; i < 3; i++){
        uint8_t *sh = opt + 0xe0 + 40 * i;
        memcpy(sh, sn[i], strlen(sn[i]));
        w32(sh + 8, 0x1000); w32(sh + 12, 0x1000u * (uint32_t)(i + 1)); w32(sh + 16, 0x1000); w32(sh + 20, 0x1000u * (uint32_t)(i + 1));
    }
    f[RVA_MAIN] = 0xc3; f[RVA_WORK] = 0xc3;
    w32(f + RVA_FIXED, PREF + RVA_DATA);
    uint8_t *e = f + RVA_EXP;                                    /* one export, "Work", ordinal 1 */
    w32(e + 0x0c, RVA_EXP + 0x80); w32(e + 0x10, 1); w32(e + 0x14, 1); w32(e + 0x18, 1);
    w32(e + 0x1c, RVA_EXP + 0x40); w32(e + 0x20, RVA_EXP + 0x50); w32(e + 0x24, RVA_EXP + 0x60);
    w32(f + RVA_EXP + 0x40, RVA_WORK); w32(f + RVA_EXP + 0x50, RVA_EXP + 0x70); w16(f + RVA_EXP + 0x60, 0);
    memcpy(f + RVA_EXP + 0x70, "Work", 5); memcpy(f + RVA_EXP + 0x80, g_names[k], strlen(g_names[k]) + 1);
    w32(f + RVA_REL, 0x1000); w32(f + RVA_REL + 4, 12); w16(f + RVA_REL + 8, 0x3000u | (RVA_FIXED - 0x1000u));
    f[0x200 + k] = (uint8_t)k;                                   /* the two files differ */
    char path[512]; snprintf(path, sizeof path, "%s/%s", g_dir, g_names[k]);
    FILE *o = fopen(path, "wb");
    if (!o || fwrite(f, 1, sizeof f, o) != sizeof f){ printf("FAIL cannot write %s\n", path); exit(2); }
    fclose(o);
    uint8_t d[32]; nfsu2_sha256(f, sizeof f, d);
    for (int i = 0; i < 32; i++) snprintf(g_sha[k] + 2 * i, 3, "%02x", d[i]);
    uint32_t b = g_bases[k];
    g_info[k] = (ModInfo){ g_names[k], g_sha[k], b, PREF, IMG_SIZE, b + RVA_MAIN, b + 0x1000u, 0x1000u, IMG_SIZE };
    static const char *const in[3] = { "HeapCreate", "HeapAlloc", "HeapDestroy" };
    for (int j = 0; j < 3; j++) g_mimp[k][j] = (ImpEnt){ b + RVA_DATA + 4u * (uint32_t)j, "KERNEL32.dll", in[j], j == 2 ? 1 : 3 };
    g_fn_addr[k][0] = b + RVA_MAIN; g_fn_addr[k][1] = b + RVA_WORK;
}

static int inside(uint32_t a, uint32_t n, uint32_t lo, uint32_t hi){ return a >= lo && a + n <= hi; }

int main(int argc, char **argv){
    if (argc < 2){ fprintf(stderr, "usage: module_unit <folder>\n"); return 2; }
    g_dir = argv[1];
    g_fn_ptr[0][0] = main0; g_fn_ptr[0][1] = work0;
#if NMOD > 1
    g_fn_ptr[1][0] = main1; g_fn_ptr[1][1] = work1;
#endif
    for (int k = 0; k < NMOD; k++){
        make_dll(k);
        g_links[k] = (ModLink){ .info = &g_info[k], .imp = g_mimp[k], .imp_count = &g_mimp_count[k],
            .fn_addr = g_fn_addr[k], .fn_ptr = g_fn_ptr[k], .fn_count = &g_fn_count[k],
            .feature = "the test", .release = "the test" };
    }
    g_links[0].slot_lo = MM_SRV_LO; g_links[0].slot_hi = MM_SRV_HI;
    g_links[0].heap_base = MM_SRV_HEAP_BASE; g_links[0].heap_size = MM_SRV_HEAP_SIZE; g_links[0].heap_n = MM_SRV_HEAP_N;
#if NMOD > 1
    g_links[1].slot_lo = MM_D3DX_LO; g_links[1].slot_hi = MM_D3DX_HI;
    g_links[1].heap_base = MM_D3DX_HEAP_BASE; g_links[1].heap_size = MM_D3DX_HEAP_SIZE; g_links[1].heap_n = MM_D3DX_HEAP_N;
#endif

    /* the layout check sees both DLLs, their slots and their heaps */
#ifdef NFSU2_GAME_MW
    CHECK(memmap_check(0x00400000u, 0x00a79000u, 0) == 0, "MW's layout with %d DLLs", NMOD);
#else
    CHECK(memmap_check(0x00400000u, 0x00932000u, 0) == 0, "UG2's layout");
#endif

    /* the import index space: the exe's, then each module's */
    module_init();
    CHECK(module_count() == NMOD, "module_count %d", module_count());
    CHECK(module_imp_first(1) == g_imp_count, "module 1's first import %u", module_imp_first(1));
    CHECK(module_of_import(g_imp_count - 1) == 0 && module_of_import(g_imp_count) == 1 && module_of_import(g_imp_count + 2) == 1,
          "import indices of the exe and module 1");
#if NMOD > 1
    CHECK(module_imp_first(2) == g_imp_count + 3, "module 2's first import %u", module_imp_first(2));
    CHECK(module_of_import(g_imp_count + 3) == 2 && module_of_import(g_imp_count + 5) == 2, "import indices of module 2");
#endif
    CHECK(module_link(0) == 0 && module_link(NMOD + 1) == 0 && module_link(1) == &g_links[0], "module_link bounds");

    /* LoadLibraryA of each, by the names a game uses */
    CPU cpu; memset(&cpu, 0, sizeof cpu); cpu.esp = 0x00C10000u;
    static const char *const ask[2] = { "SERVER", "D3DX9_26.DLL" };
    uint32_t h[NMOD];
    for (int k = 0; k < NMOD; k++){
        uint32_t err = 99;
        CHECK(module_known_name(ask[k]), "%s is a lifted module", ask[k]);
        h[k] = module_load(&cpu, ask[k], &err);
        CHECK(h[k] == g_bases[k] && err == 0, "LoadLibrary(%s) -> 0x%08x err %u", ask[k], h[k], err);
        CHECK(MR32(g_bases[k] + RVA_FIXED) == g_bases[k] + RVA_DATA, "%s relocated: 0x%08x", g_names[k], MR32(g_bases[k] + RVA_FIXED));
        for (uint32_t j = 0; j < 3; j++)
            CHECK(MR32(g_bases[k] + RVA_DATA + 4u * j) == IMP_MAGIC + module_imp_first(k + 1) + j,
                  "%s IAT slot %u bound to 0x%08x", g_names[k], j, MR32(g_bases[k] + RVA_DATA + 4u * j));
        CHECK(g_main_calls[k][1] == 1, "%s's DllMain(PROCESS_ATTACH) ran once", g_names[k]);
        CHECK(module_by_name(g_names[k]) == g_bases[k], "GetModuleHandle(%s)", g_names[k]);
    }

    /* each DLL's heap lies in its own part of the modheap region, and the parts are apart */
    for (int k = 0; k < NMOD; k++){
        const ModLink *l = module_link(k + 1);
        uint32_t lo = l->heap_base, hi = l->heap_base + l->heap_size * (uint32_t)l->heap_n;
        printf("module_unit: %s heap 0x%08x, its part 0x%08x-0x%08x, blocks 0x%08x 0x%08x 0x%08x 0x%08x\n",
               g_names[k], g_heap[k], lo, hi, g_block[k][0], g_block[k][1], g_block[k][2], g_block[k][3]);
        CHECK(g_heap[k] == lo && modheap_owns(g_heap[k]), "%s's heap 0x%08x at the start of its part", g_names[k], g_heap[k]);
        CHECK(inside(lo, hi - lo, MM_MODHEAP_BASE, MM_MODHEAP_BASE + MM_MODHEAP_SIZE * MM_MODHEAP_N), "%s's part in the region", g_names[k]);
        static const uint32_t sizes[4] = { 24, 4096, 5000, 300000 };
        for (int i = 0; i < 4; i++)
            CHECK(g_block[k][i] && inside(g_block[k][i], sizes[i], lo, hi), "%s block %d 0x%08x in its part", g_names[k], i, g_block[k][i]);
        CHECK(MR32(g_block[k][0]) == 0 && MR32(g_block[k][0] + 20) == 0, "HEAP_ZERO_MEMORY block zeroed");
        for (int j = 0; j < k; j++){
            const ModLink *o = module_link(j + 1);
            uint32_t olo = o->heap_base, ohi = o->heap_base + o->heap_size * (uint32_t)o->heap_n;
            CHECK(hi <= olo || ohi <= lo, "%s's part 0x%08x-0x%08x apart from %s's 0x%08x-0x%08x", g_names[k], lo, hi, g_names[j], olo, ohi);
        }
    }
#if NMOD > 1
    /* a module's blocks are not another module's heap's */
    CHECK(modheap_free(g_heap[1], g_block[0][1]) == 0, "a server.dll block freed on the d3dx heap is refused");
    CHECK(modheap_free(g_heap[0], g_block[0][1]) == 1, "and freed on its own");
    /* a module with all its heaps in use gets none of the other module's */
    for (int k = 0; k < NMOD; k++){
        uint32_t extra = modheap_create(k + 1);
        CHECK(extra == 0, "%s's second HeapCreate -> 0x%08x (it has %d heap)", g_names[k], extra, module_link(k + 1)->heap_n);
    }
#else
    /* UG2: server.dll has two heaps, the second one spare */
    uint32_t h2 = modheap_create(1), h3 = modheap_create(1);
    CHECK(h2 == MM_MODHEAP_BASE + MM_MODHEAP_SIZE && h3 == 0, "server.dll's 2nd and 3rd HeapCreate -> 0x%08x 0x%08x", h2, h3);
    CHECK(h2 && modheap_destroy(h2), "the spare heap destroyed");
#endif

    /* GetProcAddress and a call into each DLL's lifted code */
    for (int k = 0; k < NMOD; k++){
        uint32_t err = 99, nm = 0x00C20000u;            /* the name, in committed guest memory */
        mem_commit(nm, 16); memcpy(g_mem + nm, "Work", 5);
        uint32_t p = module_proc(h[k], nm, &err), po = module_proc(h[k], 1, &err);
        CHECK(p == g_bases[k] + RVA_WORK && po == p, "%s GetProcAddress(Work) 0x%08x, ordinal 1 0x%08x", g_names[k], p, po);
        uint32_t a = 7, r = call_guest(&cpu, p, 1, &a);
        CHECK(r == 100u * (uint32_t)(k + 1) + 7u, "%s Work(7) -> %u", g_names[k], r);
        const char *mn = 0;
        uint32_t near = module_nearest_fn(p + 5, &mn);
        CHECK(near == p && mn && !strcmp(mn, g_names[k]), "%s nearest function 0x%08x in %s", g_names[k], near, mn ? mn : "-");
        char fn[300] = "";
        CHECK(module_file_name(h[k], fn, sizeof fn) && !strcmp(fn + strlen(g_game.vroot) + 1, g_names[k]), "GetModuleFileName %s", fn);
    }

    /* FreeLibrary: DllMain(PROCESS_DETACH) destroys the heap; a new load starts from the same pages */
    for (int k = 0; k < NMOD; k++){
        CHECK(module_free(&cpu, h[k]) == 1 && g_main_calls[k][0] == 1 && g_heap[k] == 0, "%s unloaded, its heap destroyed", g_names[k]);
        CHECK(module_by_name(g_names[k]) == 0, "%s no longer loaded", g_names[k]);
    }
    uint32_t err = 99;
    uint32_t again = module_load(&cpu, g_names[0], &err);
    CHECK(again == g_bases[0] && g_heap[0] == module_link(1)->heap_base, "%s loaded again, heap 0x%08x", g_names[0], g_heap[0]);
    module_free(&cpu, again);

    /* the pin: the same file with its middle byte changed is not mapped (ERROR_BAD_EXE_FORMAT) and its
     * DllMain does not run */
    {
        char path[512]; snprintf(path, sizeof path, "%s/%s", g_dir, g_names[0]);
        FILE *f = fopen(path, "r+b"); uint8_t b = 0;
        if (!f || fseek(f, IMG_SIZE / 2, SEEK_SET) || fread(&b, 1, 1, f) != 1){ printf("FAIL cannot read %s\n", path); exit(2); }
        b ^= 1;
        if (fseek(f, IMG_SIZE / 2, SEEK_SET) || fwrite(&b, 1, 1, f) != 1){ printf("FAIL cannot write %s\n", path); exit(2); }
        fclose(f);
        uint32_t attach = g_main_calls[0][1]; err = 99;
        uint32_t bad = module_load(&cpu, g_names[0], &err);
        CHECK(bad == 0 && err == 193u && g_main_calls[0][1] == attach && module_by_name(g_names[0]) == 0,
              "%s with one byte changed: LoadLibrary -> 0x%08x err %u, not mapped", g_names[0], bad, err);
    }

    module_report(); modheap_report();
    fflush(stderr);
    printf("module_unit: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
