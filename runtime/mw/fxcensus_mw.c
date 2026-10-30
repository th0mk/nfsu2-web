/* Most Wanted's effect census (TRACKER M3-05, GAME.md Q-13): what the game
 * asks of its 31 effects and what the D3DX oracle answers, as log lines.
 * Only in a build with the oracle linked (NFSU2_D3DX_ORACLE, game_mw.c), and
 * only with NFSMW_FXCENSUS=1; otherwise dispatch.c never calls it.
 *
 * speed.exe v1.3 sets each effect up in 0x6D5C10 (this = the game's effect
 * wrapper: +4 its id, +0x48 the ID3DXEffect; the wrappers are the array
 * 0x93DE78, by id): it creates the effect (0x6C6080), asks its
 *   0x6D5D51  GetDesc(effect, &desc)                       (vtable +0xC)
 * and walks its techniques by index, up to desc.Techniques:
 *   0x6D5D69  GetTechnique(effect, index)                  (vtable +0x30)
 *   0x6D5D9D  GetInt(effect, annotation, &v) of the annotation "shader"
 *   0x6D5DC1  and of "shadowLevel", each 0 when the technique has none
 *             (GetAnnotationByName +0x4C first; GetInt +0x6C)
 *   then its own filters (a shader-level cap [0x8FAD24], -1 = none; vendor
 *   and device checks on [0x93D898]/[0x93D89C] and the caps' pixel shader
 *   version [0x93D9E4]; on NVIDIA below ps_3_0 the byte [0x8FAD9C], 1 unless
 *   the command line's first argument is "/n" (0x6CB43F), keeps only the
 *   techniques with shader <= 1), and for a technique that passes them
 *   0x6D5EB6  ValidateTechnique(effect, technique)        (vtable +0xF0)
 *   a technique that validates is kept in the wrapper's tables (a switch on
 *   the effect id, 0x6D5EC4; some cases end the walk there).
 * The draws pick one by index: GetTechnique, then SetTechnique (vtable +0xE8;
 * 0x6D75A9, 0x6E02DE and 0x6E0379 are the only `call [reg+0xE8]` in
 * 0x6B0000-0x6F0000).  An effect made again (a device reset) is not walked
 * again: the game keeps the indexes.
 *
 * Lines (the scan's calls told apart by their return address; GetTechnique
 * and SetTechnique elsewhere by their target, the oracle's functions the
 * first effect's vtable names):
 *   [fxcensus] caps vs=<VertexShaderVersion> ps=<PixelShaderVersion> vendor=.. device=.. cap=[0x8FAD24] gate=[0x8FAD9C]
 *   [fxcensus] effect e=<id> <IDI_*_FX> parameters=<n> techniques=<n> hr=<GetDesc's HRESULT>
 *   [fxcensus] tech e=<id> <IDI_*_FX> i=<index> name=<technique> passes=<n> p<k>=<vs>/<ps>... shader=<n> shadowLevel=<n> h=<handle>
 *              (every technique, asked of the oracle after GetDesc: each pass's shaders from GetPassDesc
 *              as version tokens, "-" for none; the annotations, "-" for none)
 *   [fxcensus] walk e=<id> i=<index>                     (the game's walk reached it)
 *   [fxcensus] validate e=<id> i=<index> hr=<HRESULT>
 *   [fxcensus] set e=<id> <IDI_*_FX> i=<index> hr=<HRESULT> from <call site>   (each technique's first)
 * NFSMW_FXCENSUS=n also clears the NVIDIA gate, as `speed.exe /n` does.  A
 * run: tests/mw/scripts/boot_menu.txt (its header) with NFSMW_FXCENSUS=1 in an
 * oracle build; D3D_SM=30 (com.c) reports vs_3_0/ps_3_0 in the caps. */
#if NFSU2_D3DX_ORACLE
#include "cpu.h"
#include "comrec.h"
#include "d3dxfx_mw.h"
#include <stdio.h>
#include <stdlib.h>

int g_fxcensus;                                   /* NFSMW_FXCENSUS=1 (dispatch.c reads it); bit 1: fxmark_call */
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);   /* com.c */
uint32_t com_scratch(uint32_t n);                 /* com.c: guest memory that stays */

#define RET_GETDESC   0x006d5d54u                 /* GetDesc(effect, &desc) right after the create (vtable +0xC) */
#define RET_GETTECH   0x006d5d6cu
#define RET_VALIDATE  0x006d5ebcu
#define NAME_ANNOT_A  0x008afb50u                 /* "shader", the game's annotation names */
#define NAME_ANNOT_B  0x008afb44u                 /* "shadowLevel" */
#define FX_NAMES      0x008f9be8u                 /* the effects' resource names, stride 0x90 (GAME.md 5.2) */
#define FX_STRIDE     0x90u
#define FX_WRAPPERS   0x0093de78u                 /* the game's effect wrappers, by id */
#define FX_COUNT      31u
#define EXE_LO        0x00401000u                 /* a call from speed.exe's code */
#define EXE_HI        0x008a0000u
#define VT_TECHDESC   0x14u                       /* ID3DXEffect (d3dx9_26): GetTechniqueDesc, slot 5 */
#define VT_PASSDESC   0x18u                       /* GetPassDesc, 6 */
#define VT_GETTECH    0x30u                       /* GetTechnique, 12 */
#define VT_GETPASS    0x38u                       /* GetPass, 14 */
#define VT_ANNOTBYNAME 0x4cu                      /* GetAnnotationByName, 19 */
#define VT_GETINT     0x6cu                       /* GetInt, 27 */
#define VT_SETTECH    0xe8u                       /* SetTechnique, 58 */

/* a guest string that reads as a name, else "?" */
static const char *gstr(uint32_t p){
    if (!p || !mem_is_committed(p) || !mem_is_committed(p + 63u)) return "?";
    const char *s = (const char *)(g_mem + p); int n = 0;
    while (n < 63 && s[n] >= 0x20 && s[n] < 0x7f) n++;
    return n && !s[n] ? s : "?";
}
static const char *fx_name(uint32_t id){ return id < FX_COUNT ? gstr(MR32(FX_NAMES + id * FX_STRIDE)) : "?"; }

/* a shader's version token as "vs_1_1", "ps_2_1" (fxc's ps_2_x), or "-" */
static void shader_ver(char *b, size_t n, uint32_t fn){
    uint32_t v = fn && mem_is_committed(fn) ? MR32(fn) : 0u;
    if ((v >> 16) == 0xfffeu || (v >> 16) == 0xffffu)
        snprintf(b, n, "%s_%u_%u", (v >> 16) == 0xfffeu ? "vs" : "ps", (v >> 8) & 0xffu, v & 0xffu);
    else snprintf(b, n, fn ? "?%08x" : "-", v);
}

/* the game's GetTechnique answers: (effect, handle) -> index, the most recent first */
#define RECENT 64
static struct { uint32_t fx, h, idx; } g_recent[RECENT];
static unsigned g_nrecent;
static uint32_t g_fn_gettech, g_fn_settech;       /* the oracle's GetTechnique and SetTechnique */
static uint8_t g_said[FX_COUNT][16];              /* a technique's first SetTechnique logged */

static int effect_id(uint32_t fx){
    for (uint32_t i = 0; i < FX_COUNT; i++){
        uint32_t w = MR32(FX_WRAPPERS + i * 4u);
        if (w && mem_is_committed(w + 0x48u) && MR32(w + 0x48u) == fx) return (int)i;
    }
    return -1;
}

/* NFSMW_FXCENSUS=n: also as `speed.exe /n` (the guest's command line is only its name): the NVIDIA gate
 * [0x8FAD9C] cleared before the entry point, so every technique the other filters pass is validated */
#define GATE_NV 0x008fad9cu
/* NFSU2_DXREC (runtime/comrec.c): the game's calls into the oracle's effects as context lines between the
 * device calls they make, in d3dxfx_mw.c's form (fxmark), so a recording of the oracle and one of the HLE
 * line up.  The methods are told apart by the oracle's ID3DXEffect vtable, d3dx9_26's run of 78 code
 * pointers at 0x413EA0 (docs/mw/fx-model.md 1.1) as mapped at 0xD80000; the creators by their exports,
 * which the IAT slots hold (GAME.md 5.9; their thunks' `jmp [slot]` reaches here through dispatch_tail). */
#define FX_VT 0x00d93ea0u
void fxcensus_init(void){
    const char *e = getenv("NFSMW_FXCENSUS");
    g_fxcensus = e && *e && *e != '0';
    if (g_fxcensus && *e == 'n') MR8(GATE_NV) = 0;
    if (g_fxcensus) fprintf(stderr, "[fxcensus] on: the game's calls into the D3DX oracle's effects are logged%s\n",
                            *e == 'n' ? ", the NVIDIA gate cleared (as /n)" : "");
    if (g_comrec_on) g_fxcensus |= 2;            /* the device-call recorder's effect lines (fxmark_call) */
}

static void fxmark_call(CPU *c, uint32_t addr, void (*f)(CPU *)){
    uint32_t esp = c->esp, ret = MR32(esp), a[4];
    for (int i = 0; i < 4; i++) a[i] = MR32(esp + 4u + 4u * (uint32_t)i);
    if (ret < EXE_LO || ret >= EXE_HI){ f(c); return; }        /* the oracle's own calls */
    if (addr == MR32(0x008904c0u)){              /* D3DXCreateEffectFromResourceA(device, module, name, ...) */
        fxmark(-1, "D3DXCreateEffectFromResourceA", gstr(a[2]), 0); f(c); return; }
    if (addr == MR32(0x008904c4u)){ fxmark(-1, "D3DXCreateEffectPool", 0, 0); f(c); return; }
    int slot = -1;
    if (a[0] && mem_is_committed(a[0]) && MR32(a[0]) == FX_VT)
        for (int i = 0; i < 78; i++) if (MR32(FX_VT + 4u * (uint32_t)i) == addr){ slot = i; break; }
    if (slot <= 2){ f(c); return; }                             /* IUnknown: the HLE's are com.c's own */
    int id = effect_id(a[0]);
    char arg[16] = "";
    if (slot == 63) snprintf(arg, sizeof arg, "%x", a[2]);      /* Begin: its flags */
    else if (slot == 64) snprintf(arg, sizeof arg, "%u", a[1]); /* BeginPass: the pass */
    fxmark(id, fxmark_name(slot), arg[0] ? arg : 0, 0);
    f(c);
}

/* the scan met technique `idx` (handle h) of effect `fx`: its name and each pass's shaders */
static void say_tech(CPU *c, uint32_t id, uint32_t fx, uint32_t idx, uint32_t h){
    static uint32_t buf;
    if (!buf) buf = com_scratch(64);
    uint32_t vt = MR32(fx);
    char line[512]; int k = snprintf(line, sizeof line, "[fxcensus] tech e=%u %s i=%u", id, fx_name(id), idx);
    uint32_t a[3] = { fx, h, buf };
    for (int i = 0; i < 4; i++) MR32(buf + 4u * (uint32_t)i) = 0;
    uint32_t hr = call_guest(c, MR32(vt + VT_TECHDESC), 3, a);
    uint32_t passes = hr ? 0u : MR32(buf + 4u);
    k += snprintf(line + k, sizeof line - (size_t)k, " name=%s passes=%u", hr ? "?" : gstr(MR32(buf)), passes);
    for (uint32_t p = 0; p < passes && p < 8u && k < (int)sizeof line - 40; p++){
        uint32_t ap[3] = { fx, h, p };
        uint32_t hp = call_guest(c, MR32(vt + VT_GETPASS), 3, ap);
        for (int i = 0; i < 4; i++) MR32(buf + 16u + 4u * (uint32_t)i) = 0;
        uint32_t ad[3] = { fx, hp, buf + 16u };
        char vs[16] = "?", ps[16] = "?";
        if (hp && !call_guest(c, MR32(vt + VT_PASSDESC), 3, ad)){
            shader_ver(vs, sizeof vs, MR32(buf + 24u)); shader_ver(ps, sizeof ps, MR32(buf + 28u)); }
        k += snprintf(line + k, sizeof line - (size_t)k, " p%u=%s/%s", p, vs, ps);
    }
    /* the two annotations the game picks by, "-" for none (the game reads 0 then) */
    static const uint32_t names[2] = { NAME_ANNOT_A, NAME_ANNOT_B };
    for (int n = 0; n < 2 && k < (int)sizeof line - 40; n++){
        uint32_t an[3] = { fx, h, names[n] };
        uint32_t ha = call_guest(c, MR32(vt + VT_ANNOTBYNAME), 3, an);
        uint32_t gi[3] = { fx, ha, buf + 32u };
        if (ha && !call_guest(c, MR32(vt + VT_GETINT), 3, gi))
            k += snprintf(line + k, sizeof line - (size_t)k, " %s=%d", gstr(names[n]), (int)MR32(buf + 32u));
        else k += snprintf(line + k, sizeof line - (size_t)k, " %s=-", gstr(names[n]));
    }
    fprintf(stderr, "%s h=0x%08x\n", line, h);
}

void fxcensus_call(CPU *c, uint32_t addr, void (*f)(CPU *)){
    if (!(g_fxcensus & 1)){ fxmark_call(c, addr, f); return; }      /* only the recorder's lines */
    uint32_t esp = c->esp, ret = MR32(esp), a1 = MR32(esp + 4u), a2 = MR32(esp + 8u);
    uint32_t wrap = c->esi, idx = c->ebp;
    f(c);
    if (ret < EXE_LO || ret >= EXE_HI) return;                 /* the oracle's own calls, ours */
    switch (ret){
    case RET_GETDESC: {                                         /* D3DXEFFECT_DESC: +4 Parameters, +8 Techniques */
        static int caps_said;
        if (!caps_said++)
            fprintf(stderr, "[fxcensus] caps vs=0x%08x ps=0x%08x vendor=0x%04x device=0x%04x cap=%d gate=%u\n",
                    MR32(0x0093d9e4u - 8u), MR32(0x0093d9e4u), MR32(0x0093d898u), MR32(0x0093d89cu),
                    (int)MR32(0x008fad24u), MR8(GATE_NV));
        uint32_t id = MR32(wrap + 4u), n = c->eax ? 0u : MR32(a2 + 8u);
        fprintf(stderr, "[fxcensus] effect e=%u %s parameters=%u techniques=%u hr=0x%08x\n", id, fx_name(id),
                c->eax ? 0u : MR32(a2 + 4u), n, c->eax);
        if (id < FX_COUNT && MR32(FX_WRAPPERS + id * 4u) != wrap)
            fprintf(stderr, "[fxcensus] effect %u's wrapper 0x%08x is not the array's 0x%08x\n", id, wrap, MR32(FX_WRAPPERS + id * 4u));
        for (uint32_t i = 0; i < n && i < 16u; i++){             /* every technique, walked or not */
            uint32_t at[2] = { a1, i };
            uint32_t h = call_guest(c, MR32(MR32(a1) + VT_GETTECH), 2, at);
            if (h) say_tech(c, id, a1, i, h);
        }
        return; }
    case RET_GETTECH:                                           /* the game's walk reached technique a2 */
        if (!g_fn_gettech){ g_fn_gettech = addr; g_fn_settech = MR32(MR32(a1) + VT_SETTECH); }
        fprintf(stderr, "[fxcensus] walk e=%u i=%u\n", MR32(wrap + 4u), a2);
        return;
    case RET_VALIDATE:
        fprintf(stderr, "[fxcensus] validate e=%u i=%u hr=0x%08x\n", MR32(wrap + 4u), idx, c->eax);
        return;
    default: break;
    }
    if (addr == g_fn_gettech){
        unsigned s = g_nrecent++ % RECENT;
        g_recent[s].fx = a1; g_recent[s].h = c->eax; g_recent[s].idx = a2;
    } else if (addr == g_fn_settech){
        int id = effect_id(a1), ix = -1;
        for (unsigned i = 0; i < RECENT && i < g_nrecent && ix < 0; i++){
            unsigned s = (g_nrecent - 1u - i) % RECENT;
            if (g_recent[s].fx == a1 && g_recent[s].h == a2) ix = (int)g_recent[s].idx;
        }
        static int unknown;
        if (id < 0 || ix < 0 || ix >= 16){
            if (unknown++ < 8)
                fprintf(stderr, "[fxcensus] set fx=0x%08x h=0x%08x (effect %d, index %d) hr=0x%08x from 0x%06x\n",
                        a1, a2, id, ix, c->eax, ret - 6u);
        } else if (!g_said[id][ix] && (g_said[id][ix] = 1))
            fprintf(stderr, "[fxcensus] set e=%d %s i=%d hr=0x%08x from 0x%06x\n", id, fx_name((uint32_t)id), ix, c->eax, ret - 6u);
    }
}
#endif
