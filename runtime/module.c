/* Second guest modules: game DLLs lifted next to the exe.
 *
 * The game's file lists them (game.h modules, a ModLink each).  For UG2 that
 * is one module, server.dll, EA's LAN server, which the game loads when a
 * player hosts a LAN game (SPEED2.EXE 0x5FA300: LoadLibraryA("server"),
 * GetProcAddress StartServer/StopServer/IsServerRunning, StartServer(...)).
 * gen.sh lifts each at a fixed guest base (UG2's server.dll at 0x00A00000:
 * its preferred base 0x10000000 is the guest heap) into its own gen folder,
 * with its own symbol prefix (gen/server, srv_); this file maps the ORIGINAL
 * file from the game folder at that base the way the Windows loader does:
 *
 *   1. the file must be the one the code was lifted from (SHA-256 pinned at
 *      gen time); anything else is refused and logged, hosting then fails in
 *      the game's own "problem starting the LAN server" dialog;
 *   2. headers and sections are copied, the rest of SizeOfImage is zero;
 *   3. the file's own HIGHLOW base relocations are applied for the new base,
 *      which reproduces the analysis copy the lifter saw (prepare_module.py);
 *   4. the IAT is bound to the runtime's HLE: slot j holds IMP_MAGIC + the
 *      module's first index in the combined import table + j, so a lifted
 *      `call [iat]` lands in win32_call like the exe's own imports;
 *   5. DllMain(DLL_PROCESS_ATTACH) runs on the loading thread, with the guest
 *      lock held (Windows: under the loader lock);
 *   6. FreeLibrary of the last reference runs DllMain(DLL_PROCESS_DETACH).
 *      The pages stay committed, so a thread that is still inside the module
 *      reads stale data instead of faulting; the next load copies a fresh
 *      image, so .data and BSS start over as on Windows.
 *
 * Module handles are base addresses.  The DLL's exports are cdecl, its
 * imports stdcall; nothing about calling conventions differs from the exe. */
#include "cpu.h"
#include "game.h"
#include "module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#ifdef __wasm__
void nfsu2_sha256(const void *data, size_t n, uint8_t out[32]);   /* runtime/web/sha256.c: no CommonCrypto */
#define CC_SHA256_DIGEST_LENGTH 32
#define CC_SHA256(b, n, d) nfsu2_sha256((b), (n), (d))
#define CC_LONG size_t
#else
#include <CommonCrypto/CommonDigest.h>
#endif
#include <fcntl.h>
#include <unistd.h>

extern const ImpEnt g_imp[];     extern const uint32_t g_imp_count;
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
void mem_commit(uint32_t, uint32_t);
int fileio_resolve_game(const char *win, char *out, size_t outsz);

#define IMP_MAGIC 0xF0000000u
#define DLL_PROCESS_DETACH 0u
#define DLL_PROCESS_ATTACH 1u

typedef struct {
    const ModLink *l;               /* the game's entry (game.h modules) */
    const ModInfo *info;            /* and its tables */
    const ImpEnt *imp; const uint32_t *imp_count;
    const uint32_t *fn_addr; const mod_fn *fn_ptr; const uint32_t *fn_count;
    uint32_t imp_first;             /* its first index in the import table (win32.c) */
    uint32_t *idx;                  /* .text offset -> fn index + 1 */
    int refs, mapped, refused;
    char path[1024];                /* host file it was mapped from */
    uint64_t loads, unloads;
    /* sockets the module opened and has not closed (guest SOCKET handles),
     * and the process's open host fds when it was loaded: what an unload
     * leaves behind is logged */
    uint32_t socks[256]; int nsocks, fds_at_load;
} GuestMod;

#define MOD_MAX 4
static GuestMod g_mods[MOD_MAX];
static int g_nmod, g_mods_init;
#define NMOD g_nmod

void module_init(void){
    if (g_mods_init) return;
    g_mods_init = 1;
    int n = g_game.nmodules;
    if (n > MOD_MAX){
        fprintf(stderr, "[module] %s lists %d lifted DLLs, the runtime holds %d (module.c MOD_MAX): the rest are left out\n",
                g_game.id, n, MOD_MAX);
        n = MOD_MAX;
    }
    uint32_t first = g_imp_count;
    for (int i = 0; i < n; i++){
        const ModLink *l = &g_game.modules[i];
        GuestMod *m = &g_mods[i];
        m->l = l; m->info = l->info;
        m->imp = l->imp; m->imp_count = l->imp_count;
        m->fn_addr = l->fn_addr; m->fn_ptr = l->fn_ptr; m->fn_count = l->fn_count;
        m->imp_first = first; first += *l->imp_count;
    }
    g_nmod = n;
}
int module_count(void){ return g_nmod; }
const ModLink *module_link(int m){ return m >= 1 && m <= g_nmod ? g_mods[m - 1].l : 0; }

static int mod_lifted(const GuestMod *m){ return m->info->name[0] && *m->fn_count; }

static int host_fds_open(void){
#ifdef __wasm__
    return 0;                   /* no host fds behind a guest socket in the browser (runtime/web/net_lan.c) */
#else
    int n = 0, max = getdtablesize();
    for (int fd = 0; fd < max && fd < 4096; fd++) n += fcntl(fd, F_GETFD) != -1;
    return n;
#endif
}
/* win32_call, after a ws2_32 import of a lifted module returned */
void module_note_socket_call(int mod, const char *name, uint32_t arg0, uint32_t r){
    if (mod < 1 || mod > NMOD) return;
    GuestMod *m = &g_mods[mod - 1];
    if ((!strcmp(name, "socket") || !strcmp(name, "accept")) && r != 0xFFFFFFFFu && r){
        if (m->nsocks < 256) m->socks[m->nsocks++] = r;
    } else if (!strcmp(name, "closesocket") && r == 0){
        for (int i = 0; i < m->nsocks; i++)
            if (m->socks[i] == arg0){ m->socks[i] = m->socks[--m->nsocks]; break; }
    }
}

static void build_index(GuestMod *m){
    if (m->idx || !mod_lifted(m)) return;
    const ModInfo *mi = m->info;
    m->idx = calloc(mi->text_size, sizeof *m->idx);
    if (!m->idx){ fprintf(stderr, "[module] %s: no memory for the dispatch index\n", mi->name); return; }
    for (uint32_t i = 0; i < *m->fn_count; i++){
        uint32_t off = m->fn_addr[i] - mi->text;
        if (off < mi->text_size) m->idx[off] = i + 1;
    }
}

/* "server", "SERVER.DLL", "C:\\x\\server.dll" -> "server.dll" (Windows adds
 * ".dll" when the name has no extension; a trailing dot means none). */
static void norm_name(const char *in, char *out, size_t n){
    const char *b = in;
    for (const char *p = in; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
    size_t k = 0;
    for (; b[k] && k + 5 < n; k++) out[k] = (char)tolower((unsigned char)b[k]);
    out[k] = 0;
    if (k && out[k-1] == '.') out[k-1] = 0;
    else if (!strchr(out, '.') && k + 5 < n) strcat(out, ".dll");
}
static GuestMod *by_name(const char *name){
    char nm[260]; norm_name(name, nm, sizeof nm);
    for (int i = 0; i < NMOD; i++) if (g_mods[i].info->name[0] && !strcmp(nm, g_mods[i].info->name)) return &g_mods[i];
    return 0;
}
static GuestMod *by_handle(uint32_t h){
    for (int i = 0; i < NMOD; i++) if (g_mods[i].info->name[0] && h == g_mods[i].info->base) return &g_mods[i];
    return 0;
}

static void hexsha(const uint8_t *buf, size_t n, char out[65]){
    uint8_t d[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(buf, (CC_LONG)n, d);
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(out + 2*i, 3, "%02x", d[i]);
}

static uint32_t rd32(const uint8_t *p){ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p){ return (uint16_t)(p[0] | p[1] << 8); }

/* Map, relocate and bind.  Returns 0 and sets *err (Win32 error) on failure. */
static int map_image(GuestMod *m, uint32_t *err){
    const ModInfo *mi = m->info;
    char host[1024];
    if (!fileio_resolve_game(mi->name, host, sizeof host)){
        fprintf(stderr, "[module] %s: not found in the game folder: %s is not available\n", mi->name, m->l->feature);
        *err = 126u; return 0;                                   /* ERROR_MOD_NOT_FOUND */
    }
    FILE *f = fopen(host, "rb");
    if (!f){ fprintf(stderr, "[module] %s: cannot read %s\n", mi->name, host); *err = 126u; return 0; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = sz > 0 ? malloc((size_t)sz) : 0;
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz){ fclose(f); free(buf); *err = 126u; return 0; }
    fclose(f);
    char sha[65]; hexsha(buf, (size_t)sz, sha);
    if (strcmp(sha, mi->sha256)){
        if (!m->refused++)
            fprintf(stderr, "[module] %s in the game folder (%s, %ld bytes, sha256 %s) is not the file this build"
                            " was lifted from (sha256 %s, %u bytes): it is NOT loaded, %s is disabled."
                            " Restore the original %s of %s, or re-run gen.sh with this file.\n",
                    mi->name, host, sz, sha, mi->sha256, mi->file_size, m->l->feature, mi->name, m->l->release);
        free(buf); *err = 193u; return 0;                        /* ERROR_BAD_EXE_FORMAT */
    }
    /* The hash pins the file, so these only guard against a broken gen. */
    uint32_t lfa = rd32(buf + 0x3c);
    const uint8_t *nt = buf + lfa, *opt = nt + 24;
    uint16_t nsec = rd16(nt + 6), optsz = rd16(nt + 20);
    uint32_t pref = rd32(opt + 28), imgsz = rd32(opt + 56), hdrsz = rd32(opt + 60), ep = rd32(opt + 16);
    if (rd32(nt) != 0x4550u || pref != mi->pref || imgsz != mi->size || mi->base + ep != mi->entry){
        fprintf(stderr, "[module] %s: PE layout does not match gen/ (base 0x%x size 0x%x entry 0x%x)\n",
                mi->name, pref, imgsz, ep);
        free(buf); *err = 193u; return 0;
    }
    uint32_t base = mi->base, delta = base - pref;
    mem_commit(base, imgsz);
    memset(g_mem + base, 0, imgsz);
    memcpy(g_mem + base, buf, hdrsz < (uint32_t)sz ? hdrsz : (uint32_t)sz);
    const uint8_t *sh = opt + optsz;
    for (int i = 0; i < nsec; i++, sh += 40){
        uint32_t vs = rd32(sh + 8), va = rd32(sh + 12), rs = rd32(sh + 16), ro = rd32(sh + 20);
        uint32_t n = rs < vs ? rs : vs;
        if (va + n > imgsz || ro + n > (uint32_t)sz){ free(buf); *err = 193u; return 0; }
        memcpy(g_mem + base + va, buf + ro, n);
    }
    free(buf);
    /* base relocations (DataDirectory[5]) */
    uint32_t rva = MR32(base + lfa + 24 + 96 + 5*8), rsz = MR32(base + lfa + 24 + 96 + 5*8 + 4);
    uint32_t nfix = 0;
    for (uint32_t off = 0; off + 8 <= rsz; ){
        uint32_t page = MR32(base + rva + off), bsz = MR32(base + rva + off + 4);
        if (bsz < 8) break;
        for (uint32_t k = 8; k + 2 <= bsz; k += 2){
            uint16_t e = MR16(base + rva + off + k);
            uint32_t t = e >> 12, at = base + page + (e & 0xfffu);
            if (t == 0) continue;                                /* ABSOLUTE: padding */
            if (t != 3 || at + 4 > base + imgsz){
                fprintf(stderr, "[module] %s: unsupported relocation type %u at 0x%08x\n", mi->name, t, at);
                *err = 193u; return 0; }
            MR32(at) += delta; nfix++;
        }
        off += bsz;
    }
    MR32(base + lfa + 24 + 28) = base;                           /* the loader records the actual base */
    /* imports: one index space with the exe's (win32_call) */
    uint32_t first = m->imp_first;
    for (uint32_t j = 0; j < *m->imp_count; j++){
        uint32_t iat = m->imp[j].iat;
        if (iat - base >= imgsz){ *err = 193u; return 0; }
        MR32(iat) = IMP_MAGIC + first + j;
    }
    snprintf(m->path, sizeof m->path, "%s", host);
    fprintf(stderr, "[module] %s mapped at 0x%08x-0x%08x from %s (sha256 ok, %u fixups, %u imports bound)\n",
            mi->name, base, base + imgsz, host, nfix, *m->imp_count);
    return 1;
}

uint32_t module_load(CPU *c, const char *name, uint32_t *err){
    GuestMod *m = by_name(name);
    *err = 126u;
    if (!m) return 0;
    if (!mod_lifted(m)){
        fprintf(stderr, "[module] %s: not lifted into this build (gen.sh found no %s next to %s):"
                        " %s is not available\n", m->info->name, m->info->name, g_game.exe_name, m->l->feature);
        return 0;
    }
    if (m->mapped){ m->refs++; return m->info->base; }
    if (!map_image(m, err)) return 0;
    build_index(m);
    m->mapped = 1; m->refs = 1; m->loads++;
    m->nsocks = 0; m->fds_at_load = host_fds_open();
    const uint32_t args[3] = { m->info->base, DLL_PROCESS_ATTACH, 0 };
    uint32_t ok = call_guest(c, m->info->entry, 3, args);
    fprintf(stderr, "[module] %s: DllMain(PROCESS_ATTACH) -> %u (load %llu)\n", m->info->name, ok,
            (unsigned long long)m->loads);
    if (!ok){
        /* Windows: DETACH right away, then unload; LoadLibrary returns NULL */
        const uint32_t a2[3] = { m->info->base, DLL_PROCESS_DETACH, 0 };
        call_guest(c, m->info->entry, 3, a2);
        m->mapped = 0; m->refs = 0; m->unloads++;
        *err = 1114u;                                            /* ERROR_DLL_INIT_FAILED */
        return 0;
    }
    *err = 0;
    return m->info->base;
}

int module_free(CPU *c, uint32_t h){
    GuestMod *m = by_handle(h);
    if (!m) return 0;
    if (!m->mapped || m->refs <= 0) return 1;
    if (--m->refs > 0) return 1;
    const uint32_t args[3] = { m->info->base, DLL_PROCESS_DETACH, 0 };
    uint32_t r = call_guest(c, m->info->entry, 3, args);
    m->mapped = 0; m->unloads++;
    fprintf(stderr, "[module] %s: DllMain(PROCESS_DETACH) -> %u, unloaded (pages kept); %d of its sockets still open,"
                    " host fds %d (%d at load)\n", m->info->name, r, m->nsocks, host_fds_open(), m->fds_at_load);
    for (int i = 0; i < m->nsocks && i < 8; i++) fprintf(stderr, "[module]   still open: SOCKET 0x%x\n", m->socks[i]);
    return 1;
}

int module_handle(uint32_t h){ return by_handle(h) != 0; }
int module_known_name(const char *name){ return by_name(name) != 0; }
uint32_t module_by_name(const char *name){
    GuestMod *m = by_name(name);
    return m && m->mapped ? m->info->base : 0u;
}

/* GetProcAddress on a mapped module: export by name, or by ordinal when
 * the "name" is <= 0xFFFF. */
uint32_t module_proc(uint32_t h, uint32_t nm, uint32_t *err){
    GuestMod *m = by_handle(h);
    *err = 127u;                                                 /* ERROR_PROC_NOT_FOUND */
    if (!m || !m->mapped){ *err = 126u; return 0; }
    uint32_t base = m->info->base, lfa = MR32(base + 0x3c);
    uint32_t ed = MR32(base + lfa + 24 + 96), esz = MR32(base + lfa + 24 + 96 + 4);
    if (!ed) return 0;
    uint32_t E = base + ed;
    uint32_t obase = MR32(E + 0x10), nfun = MR32(E + 0x14), nnam = MR32(E + 0x18);
    uint32_t funs = base + MR32(E + 0x1c), nams = base + MR32(E + 0x20), ords = base + MR32(E + 0x24);
    uint32_t fi = 0xFFFFFFFFu;
    if (nm <= 0xFFFFu){ if (nm >= obase && nm - obase < nfun) fi = nm - obase; }
    else if (mem_is_committed(nm)){
        const char *want = (const char *)(g_mem + nm);
        for (uint32_t i = 0; i < nnam; i++)
            if (!strcmp((const char *)(g_mem + base + MR32(nams + 4*i)), want)){ fi = MR16(ords + 2*i); break; }
    }
    if (fi >= nfun) return 0;
    uint32_t frva = MR32(funs + 4*fi);
    if (!frva || (frva >= ed && frva < ed + esz)) return 0;      /* empty or forwarded */
    *err = 0;
    return base + frva;
}

int module_file_name(uint32_t h, char *out, int n){
    GuestMod *m = by_handle(h);
    if (!m || !m->mapped) return 0;
    snprintf(out, (size_t)n, "%s\\%s", g_game.vroot, m->info->name);
    return 1;
}

uint32_t module_imp_first(int m){ return m >= 1 && m <= NMOD ? g_mods[m - 1].imp_first : 0u; }
int module_of_import(uint32_t idx){
    if (idx < g_imp_count) return 0;
    for (int i = NMOD; i >= 1; i--) if (idx >= g_mods[i - 1].imp_first) return i;
    return 0;
}

mod_fn module_fn_at(uint32_t addr){
    for (int i = 0; i < NMOD; i++){
        GuestMod *m = &g_mods[i];
        uint32_t off = addr - m->info->text;
        if (off >= m->info->text_size || !m->idx) continue;
        uint32_t k = m->idx[off];
        if (!k) return 0;
        if (!m->mapped){
            static int said = 0;
            if (said++ < 4) fprintf(stderr, "[module] WARNING: call to 0x%08x in %s while it is not loaded\n",
                                    addr, m->info->name);
        }
        return m->fn_ptr[k - 1];
    }
    return 0;
}

uint32_t module_nearest_fn(uint32_t a, const char **mod){
    for (int i = 0; i < NMOD; i++){
        GuestMod *m = &g_mods[i];
        if (!mod_lifted(m) || a - m->info->text >= m->info->text_size) continue;
        uint32_t lo = 0, hi = *m->fn_count, best = 0;
        while (lo < hi){ uint32_t md = (lo + hi) / 2;
            if (m->fn_addr[md] <= a){ best = m->fn_addr[md]; lo = md + 1; } else hi = md; }
        if (mod) *mod = m->info->name;
        return best;
    }
    return 0;
}

uint32_t module_fn_from_host(const void *pc){
    uintptr_t p = (uintptr_t)pc, best = 0; uint32_t bestg = 0;
    for (int i = 0; i < NMOD; i++){
        GuestMod *m = &g_mods[i];
        for (uint32_t k = 0; k < *m->fn_count; k++){
            uintptr_t f = (uintptr_t)m->fn_ptr[k];
            if (f <= p && f > best){ best = f; bestg = m->fn_addr[k]; }
        }
    }
    return bestg;
}

void module_report(void){
    for (int i = 0; i < NMOD; i++){
        GuestMod *m = &g_mods[i];
        if (!mod_lifted(m)){ fprintf(stderr, "  module %-10s : not lifted into this build (gen.sh found none)\n", m->info->name); continue; }
        fprintf(stderr, "  module %-10s : %u functions lifted, %llu loads, %llu unloads, %s%s\n",
                m->info->name, *m->fn_count, (unsigned long long)m->loads, (unsigned long long)m->unloads,
                m->mapped ? "loaded" : "not loaded", m->refused ? ", REFUSED (sha256 mismatch)" : "");
    }
}

/* ---- test: host start/stop cycles through the game's own glue ------------
 * Script command "srvcycle N [hold_ms]" (autotest.c), on the guest main
 * thread at Present: N times the game's start of its LAN server (game.h
 * lan_*; UG2: StartLANServer 0x5FA300, which loads server.dll and starts
 * it), keep it running hold_ms with the guest lock released, then the
 * game's stop (StopServer, FreeLibrary).  Each cycle logs what
 * would leak: threads, kernel-object slots, TLS indices, heap bytes (the
 * bytes in live blocks of the game's heaps, heap.c: the reserved address
 * space would hide a leak, because freed space is reused). */
uint64_t win32_ext_threads(void); uint64_t win32_ext_threads_exited(void);
uint32_t win32_heap_live(void); int win32_tls_in_use(void); int win32_obj_live(void);
uint32_t com_scratch(uint32_t n);
void guest_sleep_ns(int64_t ns);             /* win32_ext.c (int64_t: a long is 32 bits on wasm) */
int module_test_cycle(CPU *c, int n, int hold_ms){
    static uint32_t nm = 0;
    if (!g_game.lan_start){ fprintf(stderr, "[srvcycle] the game has no LAN server glue (game.h)\n"); return n; }
    if (!nm) nm = com_scratch(64);
    int bad = 0;
    for (int i = 1; i <= n; i++){
        int was[MOD_MAX];                        /* the modules the start loads must be gone after the stop */
        for (int k = 0; k < NMOD; k++) was[k] = g_mods[k].mapped;
        uint64_t t0 = win32_ext_threads(), e0 = win32_ext_threads_exited();
        uint32_t h0 = win32_heap_live(); int tls0 = win32_tls_in_use(), ob0 = win32_obj_live();
        snprintf((char *)(g_mem + nm), 64, "CYCLE%02d", i);
        uint32_t ecx = c->ecx; c->ecx = g_game.lan_obj;
        uint32_t ok = call_guest(c, g_game.lan_start, 1, &nm) & 0xFFu;
        c->ecx = ecx;
        uint32_t isrun = MR32(g_game.lan_running), running = isrun ? call_guest(c, isrun, 0, 0) & 0xFFu : 0u;
        if (hold_ms > 0) guest_sleep_ns((int64_t)hold_ms * 1000000);
        uint32_t t1 = (uint32_t)(win32_ext_threads() - t0);
        c->ecx = g_game.lan_obj;
        call_guest(c, g_game.lan_stop, 0, 0);
        c->ecx = ecx;
        guest_sleep_ns((int64_t)100 * 1000000);  /* threads that were told to end, end */
        uint32_t ended = (uint32_t)(win32_ext_threads_exited() - e0);
        int dh = (int)(win32_heap_live() - h0), dtls = win32_tls_in_use() - tls0, dob = win32_obj_live() - ob0;
        int left = 0;
        for (int k = 0; k < NMOD; k++) left += g_mods[k].mapped && !was[k];
        int fail = !ok || !running || left || ended != t1 || dtls != 0;
        bad += fail;
        fprintf(stderr, "[srvcycle] %d/%d start=%u running=%u threads +%u started, %u ended; loaded after stop=%d;"
                        " heap %+d KB, TLS indices %+d, object slots %+d%s\n",
                i, n, ok, running, t1, ended, left, dh / 1024, dtls, dob, fail ? "  FAIL" : "");
    }
    fprintf(stderr, "[srvcycle] %d cycles, %d failed\n", n, bad);
    return bad;
}

/* tests/srvoracle.c: map and bind the module without running DllMain (the
 * differential test calls single functions of the lifted code). */
uint32_t module_map_for_test(const char *name){
    module_init();
    GuestMod *m = by_name(name);
    uint32_t err = 0;
    if (!m || !mod_lifted(m) || !map_image(m, &err)) return 0;
    build_index(m);
    m->mapped = 1; m->refs = 1;
    return m->info->base;
}
