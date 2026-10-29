/* The D3DX oracle without a boot: Microsoft's d3dx9_26.dll lifted into
 * gen/mw/d3dx (dev only, PLAN D-09), loaded and called as Most Wanted's boot
 * loads and calls it.
 *
 *   d3dx_oracle_unit.wasm <speed.exe> <d3dx9_26.dll>        its effect-creation path (TRACKER M2-07)
 *   d3dx_oracle_unit.wasm <speed.exe> <d3dx9_26.dll> math   the native D3DX math against it (M2-08)
 *   d3dx_oracle_unit.wasm <speed.exe> <d3dx9_26.dll> boot   the boot's binding of the exe's slots alone
 * K=V arguments after the mode are set in the program's environment
 * (NFSMW_D3DX_MATH=oracle: the math left to the oracle).
 *
 * The runtime's own files do the work: mem.c maps speed.exe (its resources
 * hold the 31 effects), fileio.c, res.c, heap.c, dispatch.c, win32.c,
 * win32_ext.c, win32_misc.c, keyboard.c, module.c and modheap.c, MW's table
 * with the oracle's entry (game_mw.c, registry_mw.c, fe_mw.c, threads_mw.c),
 * its native D3DX math (d3dxmath_mw.c) and the oracle's C runtime
 * (msvcrt_mw.c); the exe's import table is gen/mw/all's, the oracle's code
 * GAME=mw web/build.sh's objects.  What is not linked is stubbed below; the
 * Direct3D device is a stand-in that records which of its methods the
 * oracle calls (com_call), with the caps com.c reports.
 *
 * In order, as the game's boot does it: win32_init binds the exe's IAT; the
 * game's install answers the 11 math imports natively (each slot to its
 * thunk, the thunk to d3dxmath_mw.c); module_load_imports (main_web.c's call
 * before the entry point) maps the oracle from NFSMW_D3DX, runs its DllMain
 * and binds the exe's other two d3dx9_26 slots to its exports.  Then
 * D3DXCreateEffectPool through the exe's slot 0x8904C4, as the game keeps
 * the pool (0x982C84), and for each of the 31 entries of the exe's effect
 * table (0x8F9BE8, stride 0x90) the call the game makes at 0x6C60D2 through
 * the slot 0x8904C0: D3DXCreateEffectFromResourceA (device, NULL, name,
 * NULL, NULL, 0, pool, &effect, &errors); or with "math" the cases of
 * d3dxmath_cases.h (math_run below).  Results are "T" lines, a failed check
 * "FAIL", the end "d3dx_oracle_unit: <n> passed, <m> failed". */
#include "cpu.h"
#include "game.h"
#include "module.h"
#include "d3d9_tables.h"
#include "mw/fx2.h"
#include "mw/d3dxmath_mw.h"
#include "d3dxmath_cases.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pass, fail;
#define CHECK(cond, ...) do { if (cond) pass++; else { fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while (0)

#define TEB_MAIN  0x00C00000u            /* memmap.h */
#define STACK_LO  0x05000000u
#define STACK_HI  0x05FF0000u
#define SCR       0x07F00000u            /* the test's own guest memory: the device, out-pointers */
#define IAT_POOL  0x008904C4u            /* speed.exe's IAT slots of D3DXCreateEffectPool, */
#define IAT_FROMRES 0x008904C0u          /* D3DXCreateEffectFromResourceA */
#define IAT_QPC   0x0089017Cu            /* speed.exe's IAT slot of QueryPerformanceCounter */
#define FX_TABLE  0x008F9BE8u            /* the exe's effect table: +0 the resource name */
#define FX_STRIDE 0x90u
#define FX_COUNT  31

/* ---- what the linked files need of the rest of the runtime ---- */
typedef void (*guest_fn)(CPU *);
const uint32_t g_fn_addr[1]; const guest_fn g_fn_ptr[1]; const uint32_t g_fn_count;   /* the exe's code: not linked */
void GAMEINFO_ENTRY_FN(CPU *c){ (void)c; }
/* the race aids (career_mw.c) and the debugging hook (debug_mw.c) the table names: not linked here */
void mw_frame(CPU *c){ (void)c; }
int  mw_debug_hook(CPU *c, uint32_t va){ (void)c; (void)va; return 0; }
const ModInfo g_srv_modinfo; const ImpEnt g_srv_imp[1]; const uint32_t g_srv_imp_count;
const uint32_t g_srv_fn_addr[1]; const mod_fn g_srv_fn_ptr[1]; const uint32_t g_srv_fn_count;
int g_efx_stat; int efx_watched(uint32_t a){ (void)a; return 0; } void efx_dispatch(CPU *c, uint32_t a){ (void)c; (void)a; }
int run_generated(CPU *c, uint32_t a){ (void)c; (void)a; return 0; }
void genexec_report(void){ }
static unsigned g_traps; static uint32_t g_trap_at[8];
void autotest_note_trap(uint32_t a, const char *mn){ if (g_traps < 8) g_trap_at[g_traps] = a; g_traps++; fprintf(stderr, "[test] trap at 0x%08x (%s)\n", a, mn); }
static char g_unimpl[64][64]; static unsigned g_nunimpl;
void autotest_note_unimpl(const char *dll, const char *name, uint32_t eip){
    for (unsigned i = 0; i < g_nunimpl; i++) if (!strcmp(g_unimpl[i], name)) return;
    if (g_nunimpl < 64) snprintf(g_unimpl[g_nunimpl++], 64, "%s", name);
    fprintf(stderr, "[test] not implemented: %s!%s (from 0x%08x)\n", dll, name, eip);
}
void autotest_note_screen(const char *nm){ (void)nm; }
uint32_t com_direct3dcreate9(void){ return 0; }
uint32_t com_directinput8create(uint32_t a){ (void)a; return 0; }
uint32_t com_directsoundcreate(uint32_t a){ (void)a; return 0; }
void com_init(void){ }
void com_forget_cpu(CPU *c){ (void)c; }
uint32_t com_scratch(uint32_t n){ (void)n; return 0; }
void watchdog_start(void){ }
int winsock_call(CPU *c, const char *dll, const char *n, uint32_t *r){ (void)c; (void)dll; (void)n; (void)r; return 0; }
void winsock_report(void){ }
int memmap_check(uint32_t lo, uint32_t hi, int fatal){ (void)lo; (void)hi; (void)fatal; return 0; }
void overrides_report(void){ } void enh_report(void){ } void d3dx_hle_report(void){ } void discord_shutdown(void){ }
long metal_frames(void){ return 0; }
void metal_game_size(int *w, int *h){ if (w) *w = 640; if (h) *h = 480; }
void metal_get_cursor(int *x, int *y){ if (x) *x = 0; if (y) *y = 0; }
int hfr_display_live(void){ return 1; } int hfr_mode_off(void){ return 1; } uint64_t hfr_vclock_floor_us(void){ return 4000u; }
const char *language_registry(void){ return 0; }
void garage_profile_written(const char *p){ (void)p; } void garage_profile_deleted(const char *p){ (void)p; }   /* fileio.c's, of UG2's garage.c */
void lifecycle_pump(void){ } int lifecycle_may_park(void){ return 0; } int lifecycle_game_parked(void){ return 0; }
void audio_set_muted(int m){ (void)m; } double audio_time(void){ return 0; }
void watch_arm(uint32_t a){ (void)a; }
long autotest_frame(void){ return 0; }
int g_stackprobe; void stackprobe_check(CPU *c){ (void)c; }
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args){   /* com.c's */
    if (!fn) return 0;
    CPU save = *c;
    for (int i = nargs - 1; i >= 0; i--) PUSH32(args[i]);
    PUSH32(0xDEADBEEFu);
    dispatch_indirect(c, fn);
    uint32_t ret = c->eax; *c = save; return ret;
}
int mem_load_pe(const char *path); void mem_commit(uint32_t, uint32_t);
void dispatch_init(void); void dispatch_report(void);
void win32_init(void); void win32_report(void);
void fileio_set_root(const char *exe_path);
void guest_lock(CPU *c);
extern uint64_t g_stat_indirect_miss, g_stat_unsupported;

/* ---- the Direct3D device's stand-in: COM addresses 0xE0000000 | interface << 8 | method ---- */
enum { IF_DEVICE = 1, IF_VSHADER, IF_PSHADER, IF_VDECL, IF_STATEBLOCK, IF_QUERY };
static const Method *iface_methods(uint32_t i, uint32_t *n){
    switch (i){
    case IF_DEVICE: *n = sizeof M_DEVICE / sizeof *M_DEVICE; return M_DEVICE;
    case IF_VSHADER: case IF_PSHADER: *n = sizeof M_VSHADER / sizeof *M_VSHADER; return M_VSHADER;
    case IF_VDECL: *n = sizeof M_VDECL / sizeof *M_VDECL; return M_VDECL;
    case IF_STATEBLOCK: *n = sizeof M_STATEBLOCK / sizeof *M_STATEBLOCK; return M_STATEBLOCK;
    case IF_QUERY: *n = sizeof M_QUERY / sizeof *M_QUERY; return M_QUERY;
    default: *n = 0; return 0;
    }
}
static uint32_t g_scr = SCR;
static uint32_t scr(uint32_t n){ uint32_t p = g_scr; g_scr += (n + 15u) & ~15u; memset(g_mem + p, 0, n); return p; }
static uint32_t g_vt[8];
static uint32_t make_obj(uint32_t iface, uint32_t extra){
    if (!g_vt[iface]){
        uint32_t n; iface_methods(iface, &n);
        g_vt[iface] = scr(4u * n);
        for (uint32_t k = 0; k < n; k++) MR32(g_vt[iface] + 4u * k) = 0xE0000000u | iface << 8 | k;
    }
    uint32_t o = scr(16u + extra);
    MR32(o) = g_vt[iface]; MR32(o + 4) = 1;           /* +4 references, +8 code, +12 its bytes */
    return o;
}
static uint32_t g_dev;
static unsigned g_calls[8][128], g_shaders[2];
static uint32_t A(CPU *c, int i){ return MR32(c->esp + 4u * (uint32_t)i); }
int com_call(CPU *c, uint32_t magic){
    uint32_t iface = (magic >> 8) & 0xFFu, k = magic & 0xFFu, n;
    const Method *M = iface_methods(iface, &n);
    POP32(c->eip);
    if (!M || k >= n){ fprintf(stderr, "[test] bad COM address 0x%08x\n", magic); c->eax = 0x80004001u; return 1; }
    if (!g_calls[iface][k]++) fprintf(stderr, "[test] COM %u.%s: first call (from 0x%08x)\n", iface, M[k].name, c->eip);
    const char *nm = M[k].name; uint32_t self = A(c, 0), r = 0;
    if (!strcmp(nm, "QueryInterface")){ if (A(c, 2)) MR32(A(c, 2)) = 0; r = 0x80004002u; }
    else if (!strcmp(nm, "AddRef")) r = ++MR32(self + 4);
    else if (!strcmp(nm, "Release")) r = MR32(self + 4) ? --MR32(self + 4) : 0u;
    else if (iface == IF_DEVICE){
        if (!strcmp(nm, "GetDeviceCaps")){                     /* com.c's answers that a shader user reads */
            uint32_t p = A(c, 1);
            memset(g_mem + p, 0, 0x130);
            MR32(p + 0x94) = 8; MR32(p + 0x98) = 8;            /* MaxTextureBlendStages, MaxSimultaneousTextures */
            MR32(p + 0xC4) = 0xFFFE0200u; MR32(p + 0xC8) = 256; MR32(p + 0xCC) = 0xFFFF0200u;   /* VS 2.0, 256, PS 2.0 */
            MRF32(p + 0xD0) = 8.0f;
        } else if (!strcmp(nm, "CreateVertexShader") || !strcmp(nm, "CreatePixelShader")){
            int ps = nm[6] == 'P';
            uint32_t fn = A(c, 1), cap = 0x40000u;
            fx2_shader s; fx2_shader_walk(g_mem + fn, cap, &s, 0, 0);
            uint32_t o = make_obj(ps ? IF_PSHADER : IF_VSHADER, 0);
            MR32(o + 8) = fn; MR32(o + 12) = s.stop < s.ndw ? 4u * (s.stop + 1u) : 0u;
            if (A(c, 2)) MR32(A(c, 2)) = o;
            g_shaders[ps]++;
            if (!s.known || !MR32(o + 12)) fprintf(stderr, "[test] %s: a shader without an end token at 0x%08x\n", nm, fn);
        } else if (!strcmp(nm, "CreateVertexDeclaration") || !strcmp(nm, "CreateStateBlock") || !strcmp(nm, "CreateQuery")){
            uint32_t o = make_obj(nm[6] == 'V' ? IF_VDECL : nm[6] == 'S' ? IF_STATEBLOCK : IF_QUERY, 0);
            if (A(c, 2)) MR32(A(c, 2)) = o;
        } else if (!strcmp(nm, "EndStateBlock")){ if (A(c, 1)) MR32(A(c, 1)) = make_obj(IF_STATEBLOCK, 0); }
        else if (!strncmp(nm, "Get", 3) && M[k].nargs >= 2 && strcmp(nm, "GetDeviceCaps")){
            /* a getter of an object or a value: nothing (NULL or 0) */
            uint32_t p = A(c, M[k].nargs - 1);
            if (p > 0xFFFFu && mem_is_committed(p)) MR32(p) = 0;
        }
    } else if (!strcmp(nm, "GetDevice")){ if (A(c, 1)) MR32(A(c, 1)) = g_dev; MR32(g_dev + 4)++; }
    else if (!strcmp(nm, "GetFunction")){                    /* (pData, pSizeOfData) */
        uint32_t d = A(c, 1), ps = A(c, 2), sz = MR32(self + 12);
        if (!d){ if (ps) MR32(ps) = sz; }
        else if (ps && MR32(ps) >= sz) memcpy(g_mem + d, g_mem + MR32(self + 8), sz);
        else r = 0x8876086Cu;                                  /* D3DERR_INVALIDCALL */
    }
    c->eax = r;
    c->esp += 4u * M[k].nargs;
    return 1;
}

static CPU g_cpu;
static void tr(const char *fmt, ...){
    char b[600]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("T %s\n", b);
}

/* ---- "math": the native D3DX math (d3dxmath_mw.c) against the oracle's lifted functions (TRACKER M2-08) ----
 * Each case of web/test/d3dxmath_cases.h runs twice from the same window: through the oracle's export (its
 * lifted code: the export's jmp through d3dx9_26's CPU table, filled by the first call, to the SSE kernel or
 * the x87 code) and through speed.exe's IAT slot as the game calls it (its thunk, then the native answer).
 * Compared: the whole window, eax, and that each returned to the caller with its arguments popped and
 * ebx, esi, edi, ebp, the x87 control word and top unchanged. */
#define MATH_W  0x07F80000u              /* the cases' window, in SCR (d3dxmath_unit.c's WIN: eax is in the digest) */
#define MATH_RA 0x00401000u              /* the "caller" */
typedef struct { uint32_t eax, esp, eip, ebx, esi, edi, ebp; uint16_t fcw; int32_t top; } Regs;
static void math_call(CPU *c, uint32_t target, const MathCase *mc, Regs *o){
    c->esp = STACK_HI - 0x4000u;
    c->eax = 0xA1A1A1A1u; c->ecx = 0xC1C1C1C1u; c->edx = 0xD1D1D1D1u;
    c->ebx = 0x0B0B0B0Bu; c->esi = 0x05050505u; c->edi = 0x0D0D0D0Du; c->ebp = 0x0E0E0E0Eu;
    c->fcw = mc->fcw; c->top = 5;
    for (int k = mc->nargs - 1; k >= 0; k--) PUSH32(mc->args[k]);
    PUSH32(MATH_RA);
    dispatch_indirect(c, target);
    *o = (Regs){ c->eax, c->esp, c->eip, c->ebx, c->esi, c->edi, c->ebp, c->fcw, c->top };
}
static int math_regs_ok(const Regs *r, const MathCase *mc){
    return r->esp == STACK_HI - 0x4000u && r->eip == MATH_RA && r->ebx == 0x0B0B0B0Bu && r->esi == 0x05050505u
        && r->edi == 0x0D0D0D0Du && r->ebp == 0x0E0E0E0Eu && r->fcw == mc->fcw && r->top == 5;
}
extern uint64_t g_d3dxmath_exact;
static void math_run(CPU *c, uint32_t base){
    static uint8_t before[MC_WIN], oracle[MC_WIN];
    mem_commit(MATH_W, MC_WIN);
    for (int f = 0; f < D3DXMATH_COUNT; f++){
        const D3dxMathFn *fn = &d3dxmath_fns[f];
        uint32_t nm = scr(64), err = 0;
        memcpy(g_mem + nm, fn->name, strlen(fn->name) + 1);
        uint32_t ex = module_proc(base, nm, &err), slot = MR32(fn->iat);
        CHECK(ex && !err, "%s: the oracle exports it (0x%08x, error %u)", fn->name, ex, err);
        CHECK(slot == fn->thunk, "%s: speed.exe's slot 0x%08x holds its thunk 0x%08x (0x%08x)", fn->name, fn->iat, fn->thunk, slot);
        if (!ex) continue;
        int bad = 0, regs_bad = 0; uint64_t h = MC_FNV0, exact0 = g_d3dxmath_exact;
        for (int i = 0; i < MC_CASES; i++){
            MathCase mc; Regs ro, rn;
            mc_case(f, i, MATH_W, &mc);
            memcpy(before, g_mem + MATH_W, MC_WIN);
            math_call(c, ex, &mc, &ro);
            memcpy(oracle, g_mem + MATH_W, MC_WIN); memcpy(g_mem + MATH_W, before, MC_WIN);
            math_call(c, slot, &mc, &rn);
            h = mc_fnv(mc_fnv(h, g_mem + MATH_W, MC_WIN), &rn.eax, 4);
            regs_bad += !math_regs_ok(&ro, &mc) || !math_regs_ok(&rn, &mc);
            if (!memcmp(oracle, g_mem + MATH_W, MC_WIN) && ro.eax == rn.eax) continue;
            if (bad++ < 6){
                uint32_t k = 0; while (k < MC_WIN && !memcmp(oracle + k, g_mem + MATH_W + k, 4)) k += 4;
                uint32_t wo, wn; memcpy(&wo, oracle + k, 4); memcpy(&wn, g_mem + MATH_W + k, 4);
                printf("  %s case %d (fcw 0x%04x, args 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x): window +0x%03x oracle 0x%08x"
                       " native 0x%08x, eax 0x%08x / 0x%08x\n", fn->name, i, mc.fcw, mc.args[0], mc.args[1], mc.args[2],
                       mc.args[3], mc.args[4], mc.args[5], k, wo, wn, ro.eax, rn.eax);
            }
        }
        tr("math %s: %d cases, %d differ, %d with registers off, %llu passes by the NaN rule, digest %016llx", fn->name, MC_CASES, bad,
           regs_bad, (unsigned long long)(g_d3dxmath_exact - exact0), (unsigned long long)h);
        CHECK(bad == 0, "%s: native and oracle the same on all %d cases (%d differ)", fn->name, MC_CASES, bad);
        CHECK(regs_bad == 0, "%s: each call returns to its caller, stdcall, the caller's registers kept (%d not)", fn->name, regs_bad);
    }
}

int main(int argc, char **argv){
    if (argc < 3){ fprintf(stderr, "usage: d3dx_oracle_unit <speed.exe> <d3dx9_26.dll> [math|boot]\n"); return 2; }
    int math = argc > 3 && !strcmp(argv[3], "math");
    setenv("NFSMW_D3DX", argv[2], 1);
    for (int i = 4; i < argc; i++){                     /* K=V after the mode: the program's environment (WASI gives none) */
        char kv[128]; snprintf(kv, sizeof kv, "%s", argv[i]);
        char *eq = strchr(kv, '=');
        if (eq){ *eq = 0; setenv(kv, eq + 1, 1); }
    }
    if (mem_load_pe(argv[1])){ printf("FAIL cannot load %s\n", argv[1]); return 2; }
    mem_commit(TEB_MAIN, 0x20000); mem_commit(STACK_LO, STACK_HI - STACK_LO); mem_commit(SCR, 0x00100000u);
    fileio_set_root(argv[1]);
    dispatch_init(); win32_init();
    CPU *c = &g_cpu;
    MR32(TEB_MAIN) = 0xFFFFFFFFu; MR32(TEB_MAIN + 4) = STACK_HI; MR32(TEB_MAIN + 8) = STACK_LO;
    MR32(TEB_MAIN + 0x18) = TEB_MAIN; MR32(TEB_MAIN + 0x24) = 1; MR32(TEB_MAIN + 0x2C) = TEB_MAIN + 0x1000;
    { extern CPU *g_cur_cpu; g_cur_cpu = c; }
    c->esp = STACK_HI - 0x1000u; c->fcw = 0x027F; c->fs_base = TEB_MAIN; c->gs_base = TEB_MAIN;
    guest_lock(c);

    /* the boot (main_web.c): the game's install (the D3DX math, native), then the oracle loaded before the
     * entry point and the exe's slots it does not answer bound to its exports: the two effect imports */
    if (g_game.install) g_game.install();
    module_load_imports(c);
    uint32_t base = module_by_name("d3dx9_26.dll");
    CHECK(base == 0x00D80000u, "the oracle is loaded at 0x%08x", base);
    uint32_t slot_pool = MR32(IAT_POOL), slot_res = MR32(IAT_FROMRES);
    tr("speed.exe's D3DXCreateEffectPool slot 0x%08x, D3DXCreateEffectFromResourceA slot 0x%08x", slot_pool, slot_res);
    CHECK(slot_pool >= 0x00D80000u && slot_pool < 0x00FCA000u && slot_res >= 0x00D80000u && slot_res < 0x00FCA000u,
          "the exe's slots lead into the oracle");
    /* the math slots: their thunks (the native answers), or with NFSMW_D3DX_MATH=oracle the oracle's exports */
    const char *ab = getenv("NFSMW_D3DX_MATH");
    int to_oracle = ab && !strcmp(ab, "oracle");
    for (int f = 0; f < D3DXMATH_COUNT; f++){
        uint32_t v = MR32(d3dxmath_fns[f].iat);
        CHECK(to_oracle ? v - 0x00D80000u < 0x0024A000u : v == d3dxmath_fns[f].thunk, "the exe's %s slot 0x%08x -> 0x%08x, %s",
              d3dxmath_fns[f].name, d3dxmath_fns[f].iat, v, to_oracle ? "the oracle" : "its thunk");
    }
    /* the exe's QueryPerformanceCounter after the oracle's DllMain, which calls the oracle's own import of it
     * first: answered too (win32.c once cached the first caller's import index, the DLL's, and left every call
     * of the exe's unanswered) */
    {
        uint32_t pc = scr(8); MR32(pc) = MR32(pc + 4) = 0xBAADF00Du;
        unsigned un = g_nunimpl;
        uint32_t r = call_guest(c, MR32(IAT_QPC), 1, &pc);
        CHECK(r == 1 && g_nunimpl == un && (MR32(pc) != 0xBAADF00Du || MR32(pc + 4) != 0xBAADF00Du),
              "the exe's QueryPerformanceCounter is answered after the oracle's DllMain (returned %u, counter %08x%08x)",
              r, MR32(pc + 4), MR32(pc));
    }
    if (argc > 3 && !strcmp(argv[3], "boot")){          /* the binding alone */
        fflush(stderr);
        printf("d3dx_oracle_unit: %d passed, %d failed\n", pass, fail);
        return fail ? 1 : 0;
    }

    if (math){
        math_run(c, base);
        fflush(stderr);
        printf("d3dx_oracle_unit: %d passed, %d failed\n", pass, fail);
        return fail ? 1 : 0;
    }
    /* D3DXCreateEffectPool(&pool) */
    uint32_t ppool = scr(4), ok = 0;
    uint32_t hr = call_guest(c, slot_pool, 1, &ppool);
    uint32_t pool = MR32(ppool);
    tr("D3DXCreateEffectPool -> 0x%08x, pool 0x%08x", hr, pool);
    CHECK(hr == 0 && pool, "D3DXCreateEffectPool: S_OK and a pool");

    /* the device, and the 31 effects as the game creates them */
    g_dev = make_obj(IF_DEVICE, 0);
    for (int i = 0; i < FX_COUNT; i++){
        uint32_t name = MR32(FX_TABLE + (uint32_t)i * FX_STRIDE), peff = scr(4), perr = scr(4);
        MR32(peff) = 0xBAADF00Du; MR32(perr) = 0xBAADF00Du;
        const uint32_t args[9] = { g_dev, 0, name, 0, 0, 0, pool, peff, perr };
        unsigned sh0 = g_shaders[0] + g_shaders[1];
        hr = call_guest(c, slot_res, 9, args);
        uint32_t eff = MR32(peff), err = MR32(perr);
        tr("D3DXCreateEffectFromResourceA(\"%s\") -> 0x%08x, effect %s, errors %s, %u shaders made", (const char *)(g_mem + name), hr,
           eff && eff != 0xBAADF00Du ? "set" : "none", err == 0 ? "NULL" : err == 0xBAADF00Du ? "untouched" : "a buffer",
           g_shaders[0] + g_shaders[1] - sh0);
        CHECK(hr == 0 && eff && eff != 0xBAADF00Du, "%s: S_OK and an effect", (const char *)(g_mem + name));
        ok += hr == 0;
    }
    tr("%u of %d effects created; %u vertex and %u pixel shaders; %u traps; %llu unresolved indirect calls", ok, FX_COUNT,
       g_shaders[0], g_shaders[1], g_traps, (unsigned long long)g_stat_indirect_miss);
    CHECK(ok == FX_COUNT, "all 31 effects");
    CHECK(g_traps == 0, "no trap (first at 0x%08x)", g_trap_at[0]);
    CHECK(g_stat_indirect_miss == 0, "no unresolved indirect call");
    for (unsigned i = 0; i < g_nunimpl; i++) tr("not implemented: %s", g_unimpl[i]);
    CHECK(g_nunimpl == 0, "every import on the path is answered (%u not)", g_nunimpl);
    module_report();
    fflush(stderr);
    printf("d3dx_oracle_unit: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
