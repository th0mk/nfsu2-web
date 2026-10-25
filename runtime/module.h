/* Second guest modules: game DLLs lifted next to the exe (server.dll).
 * tools/prepare_module.py writes a ModInfo for each into gen/<module>/modinfo.c;
 * runtime/module.c maps the original file, relocates and binds it, and runs
 * its DllMain. */
#ifndef MODULE_H
#define MODULE_H
#include <stdint.h>

typedef struct {
    const char *name;         /* lower-case file name, "" = not lifted into this build */
    const char *sha256;       /* of the original file the code was lifted from */
    uint32_t base;            /* guest base the code was lifted for */
    uint32_t pref;            /* the file's preferred ImageBase */
    uint32_t size;            /* SizeOfImage */
    uint32_t entry;           /* _DllMainCRTStartup, at base */
    uint32_t text, text_size; /* .text at base */
    uint32_t file_size;
} ModInfo;

/* an import of the exe or of a module (tools/argcount.py: impargs.c) */
typedef struct { uint32_t iat; const char*dll; const char*name; int nargs; } ImpEnt;

struct CPU;
typedef void (*mod_fn)(struct CPU *);

/* A lifted DLL of the build, as the game's file lists it (game.h modules).
 * The lift wrote its tables into the module's own gen folder (gen/server for
 * UG2's server.dll), every name with the module's own prefix (srv_:
 * g_srv_modinfo, g_srv_imp, g_srv_fn_addr, ...), so the modules of one build
 * never share a symbol.  The game's file places it in the guest: its slot
 * (memmap.h; memmap.c checks that the image lies inside) and the guest memory
 * of its own heaps: a HeapCreate this module calls gets one of heap_n heaps
 * of heap_size bytes from heap_base (modheap.c), apart from the other
 * modules' heaps. */
typedef struct ModLink {
    const ModInfo *info;
    const ImpEnt *imp; const uint32_t *imp_count;
    const uint32_t *fn_addr; const mod_fn *fn_ptr; const uint32_t *fn_count;
    uint32_t slot_lo, slot_hi;          /* its slot, [lo, hi) */
    uint32_t heap_base, heap_size;      /* its own heaps */
    int heap_n;
    const char *feature;                /* what the game cannot do without it: "LAN hosting" */
    const char *release;                /* the game release whose file it is: "NFSU2 v1.2" */
} ModLink;

/* The tables of the module whose names start with g_<p>:
 *   MODLINK_DECLARE(srv_);   ...   { MODLINK_TABLES(srv_), .slot_lo = ..., ... } */
#define MODLINK_DECLARE(p) \
    extern const ModInfo g_##p##modinfo; extern const ImpEnt g_##p##imp[]; extern const uint32_t g_##p##imp_count; \
    extern const uint32_t g_##p##fn_addr[]; extern const mod_fn g_##p##fn_ptr[]; extern const uint32_t g_##p##fn_count
#define MODLINK_TABLES(p) .info = &g_##p##modinfo, .imp = g_##p##imp, .imp_count = &g_##p##imp_count, \
    .fn_addr = g_##p##fn_addr, .fn_ptr = g_##p##fn_ptr, .fn_count = &g_##p##fn_count

/* win32_init, before any other call here: takes the game's modules (game.h)
 * and gives each its range of the import table */
void     module_init(void);
int      module_count(void);                           /* the modules are 1 .. module_count() */
const ModLink *module_link(int m);                     /* module m, 0 when there is none */
/* Module handles are the base addresses (as on Windows). */
uint32_t module_load(struct CPU *c, const char *name, uint32_t *err);  /* LoadLibraryA */
int      module_free(struct CPU *c, uint32_t h);                       /* FreeLibrary; 0 = not a module */
int      module_handle(uint32_t h);                    /* h is a lifted module (mapped or not) */
uint32_t module_by_name(const char *name);             /* GetModuleHandleA: base if mapped, else 0 */
int      module_known_name(const char *name);          /* name is a lifted module, mapped or not */
uint32_t module_proc(uint32_t h, uint32_t name_or_ord, uint32_t *err);  /* GetProcAddress */
int      module_file_name(uint32_t h, char *out, int n);                /* GetModuleFileNameA */
/* Import calls: one index space, the exe's imports first, then each
 * module's in turn.  The index of module m's first import, and which module
 * an import index belongs to (0 = the exe, m = module m). */
uint32_t module_imp_first(int m);
int      module_of_import(uint32_t idx);
/* dispatch.c: lifted function at a module address, or 0 */
mod_fn   module_fn_at(uint32_t addr);
uint32_t module_nearest_fn(uint32_t addr, const char **mod);  /* backtraces */
uint32_t module_fn_from_host(const void *pc);
void     module_report(void);
#endif
