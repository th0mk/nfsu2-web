/* D3DX9 effect library (statically linked into the game) -- HLE work.
 *
 * Measurement (2026-09-23): in a race frame 48% of the guest thread is
 * inside ID3DXEffect::BeginPass (vtable 0x7b6fe8, slot 64 = sub_0066acd2).
 * Most of that is not D3D calls but the CPU interpreting the state
 * expressions (preshaders):
 *
 *   BeginPass 66acd2
 *     -> 6691a1  apply the pass
 *        -> 669286  set one state (type>>24 switch, 100-byte record)
 *           -> 664a1f -> 663a32  evaluate the state expression
 *                          -> 663265 / 662ecc  gather the inputs
 *                          -> 688e79  preshader VM (x87)
 *        -> 669f30 -> 668dfd -> 668cc0 -> 664a1f ...
 *
 * Stage 0: counting wrappers (D3DXSTAT, D3DXANALYZE, CPUWIN).
 * Stage 1: sub_00664a1f state value memo (on by default, D3DXMEMO=0
 *          turns it off; D3DXVERIFY=<n> verifies by comparing memory).
 * Stage 2: sub_00669286 state set memo (on by default, D3DXMEMO2=0
 *          turns it off; D3DXVERIFY2=<n>).  Device call recording in com.c (dxrec.h).
 * Stage 3: parameter versions (sub_0065ab14 hook) + sub_006691a1 pass
 *          memo.  Verification found WRONG output -> off by default (D3DXMEMO3=1).
 *
 * NOTE: every sub_XXXX here must be in genall.py's OVERRIDE list, together
 * with the ones in runtime/overrides.c.                                  */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <mach/mach_time.h>
#include <string.h>
#include "dxrec.h"

typedef struct { const char *name; uint32_t addr; uint64_t n, ticks; } DxStat;

enum { DX_BEGINPASS, DX_PASSAPPLY, DX_STATEAPPLY, DX_664A2F, DX_EVALSTATE,
       DX_VM, DX_GATHER_A, DX_GATHER_B, DX_660A52, DX_668E0D,
       DX_BEGIN, DX_COMMIT, DX_ENDPASS, DX_END,
       DX_SETMATRIX, DX_SETVECTOR, DX_SETTEXTURE, DX_SETINTARRAY, DX_SETFLOAT,
       DX_SETVECTORARRAY, DX_N };

static DxStat g_dx[DX_N] = {
    [DX_BEGINPASS]      = {"BeginPass",        0x0066acd2u},
    [DX_PASSAPPLY]      = {"pass apply",        0x006691a1u},
    [DX_STATEAPPLY]     = {"state apply",       0x00669286u},
    [DX_664A2F]         = {"sub_00664a1f",      0x00664a1fu},
    [DX_EVALSTATE]      = {"eval expression",   0x00663a32u},
    [DX_VM]             = {"preshader VM",      0x00688e79u},
    [DX_GATHER_A]       = {"gather inputs (0)", 0x00663265u},
    [DX_GATHER_B]       = {"gather inputs (1,2)", 0x00662eccu},
    [DX_660A52]         = {"sub_00660a42",      0x00660a42u},
    [DX_668E0D]         = {"sub_00668dfd",      0x00668dfdu},
    [DX_BEGIN]          = {"Begin",             0x00664782u},
    [DX_COMMIT]         = {"CommitChanges",     0x0066ac17u},
    [DX_ENDPASS]        = {"EndPass",           0x0065adcbu},
    [DX_END]            = {"End",               0x0065ace5u},
    [DX_SETMATRIX]      = {"SetMatrix",         0x0066d12au},
    [DX_SETVECTOR]      = {"SetVector",         0x0066d2f3u},
    [DX_SETTEXTURE]     = {"SetTexture",        0x0066d7d9u},
    [DX_SETINTARRAY]    = {"SetIntArray",       0x0066e899u},
    [DX_SETFLOAT]       = {"SetFloat",          0x0066f4f1u},
    [DX_SETVECTORARRAY] = {"SetVectorArray",    0x0066ca28u},
};

long metal_frames(void);
int g_dx_in_pass = 0;          /* BeginPass/CommitChanges depth (COMSTAT) */
static int  g_dx_on = -1;
static long g_dx_from = 0, g_dx_f0 = -1;   /* D3DXSTAT=<frame>: count from that frame on */
static inline int dx_on(void){
    if (g_dx_on < 0){
        const char *e = getenv("D3DXSTAT");
        g_dx_on = e != NULL; g_dx_from = e ? atol(e) : 0;
    }
    return g_dx_on;
}
/* Reset the counters at the start of the measurement window (BeginPass only). */
static void dx_window(void){
    if (g_dx_f0 >= 0) return;
    long f = metal_frames();
    if (f < g_dx_from) return;
    for (int i = 0; i < DX_N; i++){ g_dx[i].n = 0; g_dx[i].ticks = 0; }
    g_dx_f0 = f;
}

/* Stage 9: a BeginPass that switches the effect's pass walks the pass
 * records (0x65AE25) before it applies the new one, and that may change
 * what a later application of either evaluates: it moves the epoch. */
extern uint32_t g_fx_epoch;
static inline void beginpass_note(CPU *c){
    uint32_t th = MR32(c->esp+4u);
    if (MR16(th+0x44u) != (MR32(c->esp+8u) & 0xffffu)) g_fx_epoch++;
}
#define DX_WRAP(idx, hex)                                                   \
    void sub_##hex##_orig(CPU *c);                                          \
    void sub_##hex(CPU *c){                                                 \
        if (idx == DX_BEGINPASS) beginpass_note(c);                         \
        if (!dx_on()){ sub_##hex##_orig(c); return; }                       \
        if (idx == DX_BEGINPASS) dx_window();                               \
        int inp = (idx == DX_BEGINPASS || idx == DX_COMMIT);                \
        g_dx_in_pass += inp;                                                \
        uint64_t t0 = mach_absolute_time();                                 \
        sub_##hex##_orig(c);                                                \
        g_dx_in_pass -= inp;                                                \
        g_dx[idx].ticks += mach_absolute_time() - t0;                       \
        g_dx[idx].n++;                                                      \
    }

DX_WRAP(DX_BEGINPASS,      0066acd2)
DX_WRAP(DX_VM,             00688e79)
DX_WRAP(DX_GATHER_A,       00663265)
DX_WRAP(DX_GATHER_B,       00662ecc)
DX_WRAP(DX_660A52,         00660a42)
DX_WRAP(DX_BEGIN,          00664782)
DX_WRAP(DX_COMMIT,         0066ac17)
DX_WRAP(DX_ENDPASS,        0065adcb)
DX_WRAP(DX_END,            0065ace5)
DX_WRAP(DX_SETMATRIX,      0066d12a)
DX_WRAP(DX_SETVECTOR,      0066d2f3)
DX_WRAP(DX_SETINTARRAY,    0066e899)
DX_WRAP(DX_SETFLOAT,       0066f4f1)
DX_WRAP(DX_SETVECTORARRAY, 0066ca28)

/* ---- Analysis: do the inputs of the expression evaluation really change? ----
 * D3DXANALYZE=<frame>: from that frame on, on every sub_00663a32 call, compare
 * a hash of the state's input parameter data with the previous evaluation.
 *
 * sub_00663a32(this=ecx, a, b)  ret 8
 *   pass/param record : P = a*0x64 + [this+0x2c]
 *   state record      : R = b*0x64 + [P+4]
 *   input records     : [R+0x34] + i*32, i < word[R+0x4e]   (only if [R+0x3c] != 0)
 *     +0  dirty flag (if nonzero gather, then [+0] = [+4])
 *     +0xc -> {data pointer, desc}   desc: +0 type, +4 class, +0x10 elements,
 *                                          +0x14 rows, +0x18 columns
 *     +0x10 -> register desc: word +4 set (0/1/2), +6 index, +8 count      */
typedef struct { uint32_t k0, k1; uint64_t h; uint32_t n; } AnSlot;
#define AN_CAP (1u<<16)
static AnSlot *g_an; static long long g_an_from = -1;   /* 1<<40 = never: 64-bit on wasm32 too */
static uint64_t an_calls, an_first, an_same, an_diff, an_noinput, an_dirty0, an_bytes;
static uint64_t an_hist_inputs[9];

static uint64_t fnv(const uint8_t *p, uint32_t n, uint64_t h){
    for (uint32_t i = 0; i < n; i++){ h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}
static void analyze_eval(CPU *c){
    if (g_an_from < 0){ const char *e = getenv("D3DXANALYZE"); g_an_from = e ? atoll(e) : 1LL<<40; }
    if (metal_frames() < g_an_from) return;
    if (!g_an) g_an = calloc(AN_CAP, sizeof *g_an);
    uint32_t th = c->ecx, a = MR32(c->esp+4), b = MR32(c->esp+8);
    uint32_t P = a*0x64u + MR32(th+0x2cu), R = b*0x64u + MR32(P+4u);
    an_calls++;
    uint64_t h = 0xcbf29ce484222325ull; unsigned nin = 0, alldirty0 = 1;
    if (MR32(R+0x3cu)){
        unsigned n = MR16(R+0x4eu); uint32_t base = MR32(R+0x34u);
        for (unsigned i = 0; i < n; i++){
            uint32_t rec = base + i*32u;
            if (!MR32(rec)) continue;             /* not dirty: D3DX will not gather */
            alldirty0 = 0; nin++;
            uint32_t pp = MR32(rec+0xcu), dp = MR32(pp), ds = MR32(pp+4u);
            uint32_t el = MR32(ds+0x10u); if (!el) el = 1;
            uint32_t rows = MR32(ds+0x14u); if (!rows) rows = 1;
            uint32_t bytes = el*rows*16u; if (bytes > 4096u) bytes = 4096u;
            if (dp){ h = fnv(g_mem + dp, bytes, h); an_bytes += bytes; }
            h = fnv((const uint8_t*)&dp, 4, h);
        }
    }
    an_hist_inputs[nin > 8 ? 8 : nin]++;
    if (alldirty0) an_dirty0++;
    if (!nin){ an_noinput++; }
    uint32_t hk = (th * 0x9E3779B1u) ^ (a * 0x85EBCA77u) ^ (b * 0xC2B2AE3Du);
    for (uint32_t j = 0; j < AN_CAP; j++){
        AnSlot *s = &g_an[(hk + j) & (AN_CAP-1)];
        if (s->n && (s->k0 != th || s->k1 != ((a<<16)|(b&0xffff)))) continue;
        if (!s->n){ s->k0 = th; s->k1 = (a<<16)|(b&0xffff); s->h = h; s->n = 1; an_first++;
            if (an_first <= 12){
                uint32_t o = MR32(R+0x38u), vt = MR32(o);
                fprintf(stderr, "[D3DXANALYZE] key this=%08x a=%u b=%u R=%08x obj=%08x vt=%08x vt[3]=%08x"
                        " [R+0x40]=%08x [R+0x44]=%08x P.data=%08x\n", th, a, b, R, o, vt, MR32(vt+12u),
                        MR32(R+0x40u), MR32(R+0x44u), MR32(P));
            }
            return; }
        if (s->h == h) an_same++; else an_diff++;
        s->h = h; s->n++;
        return;
    }
}
void sub_00663a32_orig(CPU *c);
void sub_00663a32(CPU *c){
    if (!dx_on()){ sub_00663a32_orig(c); return; }
    analyze_eval(c);
    uint64_t t0 = mach_absolute_time();
    sub_00663a32_orig(c);
    g_dx[DX_EVALSTATE].ticks += mach_absolute_time() - t0;
    g_dx[DX_EVALSTATE].n++;
}

/* ---- State value memo: sub_00664a1f(this=ecx, a, b)  ret 8 ----
 * Computes the state's value and writes it to P.data + [R+0x48].  For
 * expression states ([R+0x38] != 0, [R+0x30]&4 == 0) the result depends only
 * on the data of its single input (D3DXANALYZE: 95.8% identical).  If the
 * input hash did not change, skipping the call must change nothing in
 * memory -- D3DXVERIFY measures that.
 *   D3DXVERIFY=<n> : on 1/n of the predicted hits run the original and compare
 *                    the effect object +-256 KB and .data before/after.
 *   D3DXMEMO=0     : turn the memo off (default ON).
 * Extra guard: on a hit the target dword (P.data+[R+0x48]) and the first
 * dword of the value buffer ([R+8]) must be unchanged since the last real
 * computation; if the effect is recreated at the same address or someone
 * writes the target, it is recomputed. */
/* Memo record: a FULL copy of the input is kept and compared with memcmp
 * (not a hash -- no collision risk; a byte-by-byte FNV hash took 8% in the
 * profile).  Inputs larger than MEMO_BUF are not memoized, always computed. */
#define MEMO_BUF 256u
typedef struct { uint32_t th, ab, eax, n, dst, dval, vval, len, valid; uint8_t buf[MEMO_BUF]; } MemoSlot;
#define MEMO_CAP 4096u
static MemoSlot *g_memo;
static int g_memo_on = -1, g_verify_n = -1;
static uint64_t m_calls, m_elig, m_big, m_hit, m_skip, m_guard, v_runs, v_clean, v_dirty;
static uint8_t *v_buf0, *v_buf1;
#define V_WIN   0x40000u
#define V_DATA0 0x007e8000u
#define V_DATA1 0x008bc000u

/* Snapshot writer: compares in place against the OLD snapshot in the record
 * slot's buffer and copies only from the first difference on.  With a hit
 * rate of 90%+ most calls copy nothing at all (copying first and then
 * memcmp-ing took 9% in the profile). */
typedef struct { uint8_t *d; uint32_t len, cap; int eq; } SnapW;
/* Inline: XOR-OR compare in 8-byte words.  For a constant n the compiler
 * unrolls it fully; the library memcmp call was 2.6% in the profile. */
static inline __attribute__((always_inline)) int sw_put(SnapW *w, const void *p, uint32_t n){
    if (w->len + n > w->cap) return 0;
    uint8_t *q = w->d + w->len;
    const uint8_t *s = (const uint8_t *)p;
    if (w->eq){
        uint64_t x = 0; uint32_t i = 0;
        for (; i + 8u <= n; i += 8u){ uint64_t a, b; memcpy(&a, q+i, 8); memcpy(&b, s+i, 8); x |= a ^ b; }
        for (; i + 4u <= n; i += 4u){ uint32_t a, b; memcpy(&a, q+i, 4); memcpy(&b, s+i, 4); x |= a ^ b; }
        for (; i < n; i++) x |= (uint64_t)(q[i] ^ s[i]);
        if (x){ w->eq = 0; memcpy(q, s, n); }
    } else memcpy(q, s, n);
    w->len += n;
    return 1;
}

/* The evaluation's inputs; 0 if the buffer overflows. */
static inline __attribute__((always_inline)) int state_input_snap(SnapW *w, uint32_t th, uint32_t P, uint32_t R){
    uint32_t fix[6] = { th, R, MR32(R+0x38u), MR32(P), MR32(R+0x48u), MR32(R+8u) };
    if (!sw_put(w, fix, sizeof fix)) return 0;
    if (MR32(R+0x3cu)){
        unsigned n = MR16(R+0x4eu); uint32_t base = MR32(R+0x34u);
        for (unsigned i = 0; i < n; i++){
            uint32_t rec = base + i*32u;
            uint32_t pp = MR32(rec+0xcu), dp = MR32(pp), ds = MR32(pp+4u);
            uint32_t el = MR32(ds+0x10u); if (!el) el = 1;
            uint32_t rows = MR32(ds+0x14u); if (!rows) rows = 1;
            uint32_t bytes = dp ? el*rows*16u : 0u;
            uint32_t hdr[2] = { MR32(rec), dp };
            if (bytes > w->cap || !sw_put(w, hdr, sizeof hdr)) return 0;
            if (bytes && !sw_put(w, g_mem + dp, bytes)) return 0;
        }
    }
    return 1;
}

static void verify_diff(uint32_t lo, uint32_t n, const uint8_t *before,
                        uint32_t th, uint32_t P, uint32_t R){
    static int shown = 0;
    for (uint32_t i = 0; i < n && shown < 24; i++){
        if (before[i] == g_mem[lo+i]) continue;
        uint32_t a = lo + i, pd = MR32(P);
        fprintf(stderr, "[D3DXVERIFY] diff %08x  (this%+d  R%+d  P.data%+d)  %02x -> %02x\n",
                a, (int)(a-th), (int)(a-R), (int)(a-pd), before[i], g_mem[a]);
        shown++;
        i |= 3u;                       /* print the same dword once */
    }
}

static inline __attribute__((always_inline)) uint32_t rd32s(uint32_t a){ return (a && mem_is_committed(a) && mem_is_committed(a+3u)) ? MR32(a) : 0u; }

static int g_pctx_n;
static void pctx_note_inputs(uint32_t R);
void sub_00664a1f_orig(CPU *c);
void sub_00664a1f(CPU *c){
    if (g_memo_on < 0){
        const char *m = getenv("D3DXMEMO");
        g_memo_on = m ? atoi(m) != 0 : 1;
        const char *v = getenv("D3DXVERIFY"); g_verify_n = v ? atoi(v) : 0;
        if (g_memo_on || g_verify_n) g_memo = calloc(MEMO_CAP, sizeof *g_memo);
    }
    uint32_t th = c->ecx, a = MR32(c->esp+4u), b = MR32(c->esp+8u);
    uint32_t P = a*0x64u + MR32(th+0x2cu), R = b*0x64u + MR32(P+4u);
    if (g_pctx_n) pctx_note_inputs(R);     /* Stage 3: pass dependency */
    if (!g_memo){ sub_00664a1f_orig(c); return; }
    m_calls++;
    if ((MR8(R+0x30u) & 4) || !MR32(R+0x38u)){ sub_00664a1f_orig(c); return; }
    m_elig++;
    uint32_t ab = (a<<16) | (b & 0xffffu);
    uint32_t hk = (th * 0x9E3779B1u) ^ (ab * 0x85EBCA77u);
    MemoSlot *s = NULL;
    for (uint32_t j = 0; j < 64u; j++){
        MemoSlot *t = &g_memo[(hk + j) & (MEMO_CAP-1)];
        if (!t->n || (t->th == th && t->ab == ab)){ s = t; break; }
    }
    if (!s){ sub_00664a1f_orig(c); return; }
    int mine = s->n && s->th == th && s->ab == ab && s->valid;
    SnapW w = { s->buf, 0, MEMO_BUF, mine };
    if (!state_input_snap(&w, th, P, R)){
        m_big++; s->valid = 0; s->th = th; s->ab = ab; s->n++;
        sub_00664a1f_orig(c); return; }
    uint32_t dst = MR32(P) + MR32(R+0x48u), vb = MR32(R+8u);
    int same = mine && w.eq && w.len == s->len;
    int hit = same && s->dst == dst && rd32s(dst) == s->dval && rd32s(vb) == s->vval;
    if (same && !hit) m_guard++;
    if (hit){
        m_hit++;
        if (g_verify_n && (m_hit % (uint64_t)g_verify_n) == 0){
            /* window: around this, only mapped 16 KB pages */
            uint32_t lo = th & ~0x3fffu, hi = lo + 0x4000u, dn = V_DATA1 - V_DATA0;
            while (lo - 0x4000u >= th - V_WIN && mem_is_committed(lo - 0x4000u)) lo -= 0x4000u;
            while (hi + 0x4000u <= th + V_WIN && mem_is_committed(hi)) hi += 0x4000u;
            uint32_t n = hi - lo;
            if (!v_buf0){ v_buf0 = malloc(2u*V_WIN + 0x8000u); v_buf1 = malloc(dn); }
            memcpy(v_buf0, g_mem + lo, n); memcpy(v_buf1, g_mem + V_DATA0, dn);
            uint32_t esp0 = c->esp;
            sub_00664a1f_orig(c);
            v_runs++;
            /* this+0x114 (saved FPU CW) and +0x118 bit0 (CW saved):
             * 663a32 sets the FPU precision on the first evaluation inside
             * BeginPass, and the end of BeginPass restores it.  A skipped
             * evaluation never touches the CW, so no restore is needed either
             * -- a harmless difference. */
            { uint32_t off = th + 0x114u - lo; memcpy(v_buf0 + off, g_mem + th + 0x114u, 8); }
            int d0 = memcmp(v_buf0, g_mem + lo, n) != 0;
            int d1 = memcmp(v_buf1, g_mem + V_DATA0, dn) != 0;
            if (d0 || d1 || c->eax != s->eax){
                v_dirty++;
                if (d0) verify_diff(lo, n, v_buf0, th, P, R);
                if (d1) verify_diff(V_DATA0, dn, v_buf1, th, P, R);
                if (c->eax != s->eax)
                    fprintf(stderr, "[D3DXVERIFY] eax differs %08x -> %08x\n", s->eax, c->eax);
            } else v_clean++;
            (void)esp0;
            s->eax = c->eax; s->dval = rd32s(dst); s->vval = rd32s(vb);
            return;
        }
        if (g_memo_on){
            m_skip++;
            POP32(c->eip); c->esp += 8u; c->eax = s->eax;
            return;
        }
    }
    s->th = th; s->ab = ab; s->valid = 0; s->n++;     /* the buffer already holds the new snapshot */
    sub_00664a1f_orig(c);
    s->len = w.len; s->eax = c->eax; s->valid = 1;
    s->dst = dst; s->dval = rd32s(dst); s->vval = rd32s(vb);
}

/* ---- Stage 3: parameter versions ----
 * Parameter data changes only through Set*, and every Set* reaches D3DX's
 * "parameter changed" point sub_0065ab14 (20 call sites, all of them
 * vtable Set* slots + CloneEffect + the parameter block path).  There a
 * global counter is written to the 64-byte lines of the changed data.  The
 * effect pool shares data between effects, so the key is the data ADDRESS.
 * A pass's dependencies = the lines its states read; if none changed since
 * the recording, the pass produces the same output.                       */
#define PV_CAP (1u<<16)
typedef struct { uint32_t line; uint32_t ep; } PvSlot;
static PvSlot  g_pv[PV_CAP];
static uint32_t g_pepoch = 1;
static int      g_pv_on = 0;         /* keep versions only while Stage 3 is on */
static uint64_t pv_bumps, pv_full;

static inline uint32_t pv_get(uint32_t line){
    uint32_t h = line * 0x9E3779B1u;
    for (uint32_t j = 0; j < 32u; j++){
        PvSlot *t = &g_pv[(h + j) & (PV_CAP-1)];
        if (t->line == line) return t->ep;
        if (!t->line) return 0;
    }
    return 0xFFFFFFFFu;                  /* table full: always "changed" */
}
static inline void pv_set(uint32_t line, uint32_t ep){
    uint32_t h = line * 0x9E3779B1u;
    for (uint32_t j = 0; j < 32u; j++){
        PvSlot *t = &g_pv[(h + j) & (PV_CAP-1)];
        if (t->line == line || !t->line){ t->line = line; t->ep = ep; return; }
    }
    pv_full++;
}
/* Data range of a parameter slot (upper-bound estimate: 16 bytes per row,
 * a matrix 4x16).  Desc: +0x10 elements, +0x14 rows. */
static inline void slot_range(uint32_t sl, uint32_t *dp, uint32_t *n){
    uint32_t d = MR32(sl), ds = MR32(sl+4u);
    uint32_t el = MR32(ds+0x10u); if (!el) el = 1;
    uint32_t rows = MR32(ds+0x14u); if (!rows) rows = 1;
    uint32_t b = el*rows*16u; if (b > 4096u) b = 4096u;
    *dp = d; *n = d ? b : 0u;
}

/* Local 66055c verification: the 65ab14 calls the original makes directly
 * are logged in order as (effect, slot); nested calls are not logged. */
#define AB_LOG 64
static int      g_ab_log, g_ab_n;
static uint32_t g_ab_seq[AB_LOG][2];

void sub_0065ab14_orig(CPU *c);
void sub_0065ab14(CPU *c){
    uint32_t sl = MR32(c->esp+4u), dp, n;
    int lg = g_ab_log;
    if (lg){
        if (g_ab_n < AB_LOG){ g_ab_seq[g_ab_n][0] = c->ecx; g_ab_seq[g_ab_n][1] = sl; }
        g_ab_n++; g_ab_log = 0;
    }
    if (g_pv_on && sl && mem_is_committed(sl) && mem_is_committed(sl+7u)){
        slot_range(sl, &dp, &n);
        if (n){
            uint32_t ep = ++g_pepoch;
            for (uint32_t l = dp >> 6; l <= (dp + n - 1u) >> 6; l++) pv_set(l, ep);
            pv_bumps++;
        }
    }
    sub_0065ab14_orig(c);
    g_ab_log = lg;
}

/* ---- Native helpers (Option 1) ----
 * C that produces exactly the same result, in place of the lifted x87/flag code.
 *   D3DXNATIVE=0     : off (default on)
 *   D3DXNVERIFY=<n>  : every n-th call compute the native result, run the
 *                      original, compare (matrix: 64 bytes + eax;
 *                      pool: 65ab14 call sequence + eax).               */
static int g_dxn_on = -1, g_dxn_v = 0;
static uint64_t mm_calls, mm_slow, mmv_runs, mmv_bad, pl_calls, plv_runs, plv_bad;
static inline int dxn_on(void){
    if (g_dxn_on < 0){
        const char *e = getenv("D3DXNATIVE"); g_dxn_on = e ? atoi(e) != 0 : 1;
        const char *v = getenv("D3DXNVERIFY"); g_dxn_v = v ? atoi(v) : 0;
    }
    return g_dxn_on;
}

/* D3DXMatrixMultiply(out, m1, m2)  stdcall, ret 12, eax = out.
 * Original (x87): for each output p_k = a_ik*b_kj (double; the product of
 * two floats is exact), t = (p2+p0) + (p1+p3), out = (float)t.  The row path
 * (m2 != out) reads the m1 row and the column path (m2 == out, m1 != out)
 * the m2 column before writing, so the result is always the product of the
 * original inputs; with m1 == m2 == out, m2 is copied to the stack and the
 * row path is taken.  The only difference between the two paths is the
 * operand order in the multiply -- it matters only for NaN propagation.
 * Fast path: if all 32 inputs are finite the products/sums cannot produce
 * NaN or overflow (|a*b| < 2^256), and the x87_r2 checks pass trivially.
 * Otherwise, and on partial overlap, the original runs.  FMA contraction
 * must be OFF.                                                          */
#pragma clang fp contract(off)
static inline __attribute__((always_inline)) int mm_ovl(uint32_t a, uint32_t b){
    return a != b && a - b + 63u < 127u;   /* |a-b| < 64 */
}
static inline __attribute__((always_inline)) int mm_load(float *M, uint32_t ga){
    uint32_t u[16], bad = 0;
    memcpy(u, g_mem + ga, 64);
    for (int k = 0; k < 16; k++) bad |= ((u[k] & 0x7f800000u) == 0x7f800000u);
    memcpy(M, u, 64);
    return !bad;
}
static inline __attribute__((always_inline)) void mm_mul(float *R, const float *A, const float *B){
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++){
            double p0 = (double)A[i*4+0] * (double)B[0*4+j];
            double p1 = (double)A[i*4+1] * (double)B[1*4+j];
            double p2 = (double)A[i*4+2] * (double)B[2*4+j];
            double p3 = (double)A[i*4+3] * (double)B[3*4+j];
            double s1 = p2 + p0;
            double s2 = p1 + p3;
            R[i*4+j] = (float)(s1 + s2);
        }
}
void sub_0067192e_orig(CPU *c);
void sub_0067192e(CPU *c){
    if (!dxn_on()){ sub_0067192e_orig(c); return; }
    uint32_t sp = c->esp, out = MR32(sp+4u), m1 = MR32(sp+8u), m2 = MR32(sp+12u);
    float A[16], B[16], R[16];
    mm_calls++;
    if (mm_ovl(out, m1) || mm_ovl(out, m2) || !mm_load(A, m1) || !mm_load(B, m2)){
        mm_slow++; sub_0067192e_orig(c); return; }
    mm_mul(R, A, B);
    if (g_dxn_v && (mm_calls % (uint64_t)g_dxn_v) == 0){
        sub_0067192e_orig(c);
        mmv_runs++;
        if (memcmp(g_mem + out, R, 64) || c->eax != out){
            if (mmv_bad++ < 8)
                fprintf(stderr, "[D3DXNVERIFY] matrix differs out=%08x m1=%08x m2=%08x eax=%08x\n",
                        out, m1, m2, c->eax);
        }
        return;
    }
    memcpy(g_mem + out, R, 64);
    /* the original's ecx/edx on exit (meaningless to the caller, but cheap) */
    if (m2 != out){ c->ecx = m1 + 0x40u; c->edx = m2; }
    else if (m1 == out){ c->ecx = m1 + 0x40u; c->edx = sp - 0x44u; }
    else { c->ecx = m1; c->edx = m2; }
    c->eax = out;
    POP32(c->eip); c->esp += 12u;
}
#pragma clang fp contract(on)

/* Parameter-changed notification, sub_0066055c(this=ecx, slot) ret 4.
 * If the parameter is shared in the effect pool: 65ab14(effect, the slot in
 * that effect) for every active effect in the pool (byte +0x119 bit 0), then
 * for itself.
 *   pobj = [[this+0xb0] + [slot+0x14]*4],  head = [pobj+0x40]  (ring list,
 *   node: +0 effect, +4 slot, +0x10 next), pool = [this+0x38]:
 *   +8 effect count, +0xc the single effect.
 * The original's prologue (ebp/ebx/esi/edi pushes) is done exactly: 65ab14
 * sees the same esp.  65ab14 reads only ecx and [esp+4]. */
static inline __attribute__((always_inline)) uint32_t pl_call(CPU *c, uint32_t eff, uint32_t sl, uint32_t ret){
    c->ecx = eff; PUSH32(sl); PUSH32(ret); sub_0065ab14(c); return c->eax;
}
/* Dry run (verification): the sequence that is made if every call returns >= 0. */
static int pl_plan(uint32_t th, uint32_t sl, uint32_t seq[][2]){
    int n = 0;
    #define PL_ADD(e, s) do{ if (n < AB_LOG){ seq[n][0] = (e); seq[n][1] = (s); } n++; }while(0)
    uint32_t pobj = MR32(MR32(th+0xb0u) + MR32(sl+0x14u)*4u), head = MR32(pobj+0x40u);
    if (!head){ PL_ADD(th, sl); return n; }
    uint32_t pool = MR32(th+0x38u), cnt = MR32(pool+8u);
    if (cnt <= 1u){
        if (!cnt) return 0;
        uint32_t e = MR32(pool+0xcu);
        if (e == th){ PL_ADD(th, sl); return n; }
        if (e) for (uint32_t q = MR32(head+0x10u); q != head; q = MR32(q+0x10u))
            if (MR32(q) == e){ PL_ADD(e, MR32(q+4u)); return n; }
    }
    for (uint32_t q = MR32(head+0x10u); q != head; q = MR32(q+0x10u)){
        uint32_t ef = MR32(q);
        if (ef && (MR8(ef+0x119u) & 1u)) PL_ADD(ef, MR32(q+4u));
    }
    if (MR8(th+0x119u) & 1u) PL_ADD(th, sl);
    #undef PL_ADD
    return n;
}
void sub_0066055c_orig(CPU *c);
void sub_0066055c(CPU *c){
    if (!dxn_on()){ sub_0066055c_orig(c); return; }
    uint32_t th = c->ecx, sl = MR32(c->esp+4u);
    pl_calls++;
    if (g_dxn_v && (pl_calls % (uint64_t)g_dxn_v) == 0){
        static uint32_t plan[AB_LOG][2];
        int pn = pl_plan(th, sl, plan);
        int lg = g_ab_log; g_ab_log = 1; g_ab_n = 0;
        sub_0066055c_orig(c);
        g_ab_log = lg;
        plv_runs++;
        int bad = pn != g_ab_n;
        for (int i = 0; !bad && i < pn && i < AB_LOG; i++)
            bad = plan[i][0] != g_ab_seq[i][0] || plan[i][1] != g_ab_seq[i][1];
        if (!pn && c->eax) bad = 1;
        if (bad && plv_bad++ < 8)
            fprintf(stderr, "[D3DXNVERIFY] pool differs this=%08x slot=%08x plan %d calls, original %d (eax %08x)\n",
                    th, sl, pn, g_ab_n, c->eax);
        return;
    }
    uint32_t sebp = c->ebp, sebx = c->ebx, sesi = c->esi, sedi = c->edi, r;
    PUSH32(sebp); uint32_t fp = c->esp; PUSH32(sebx); PUSH32(sesi); PUSH32(sedi);
    c->ebp = fp; c->ebx = th; c->edx = sl;
    uint32_t pobj = MR32(MR32(th+0xb0u) + MR32(sl+0x14u)*4u), head = MR32(pobj+0x40u);
    c->edi = head;
    if (!head){ r = pl_call(c, th, sl, 0x6605eeu); goto done; }
    {
    uint32_t pool = MR32(th+0x38u), cnt = MR32(pool+8u);
    if (cnt <= 1u){
        if (!cnt){ r = 0; goto done; }
        uint32_t e = MR32(pool+0xcu);
        if (e == th){ r = pl_call(c, th, sl, 0x6605eeu); goto done; }
        if (e) for (uint32_t q = MR32(head+0x10u); q != head; q = MR32(q+0x10u))
            if (MR32(q) == e){ r = pl_call(c, e, MR32(q+4u), 0x6605eeu); goto done; }
    }
    for (uint32_t q = MR32(head+0x10u); q != head; q = MR32(q+0x10u)){
        uint32_t ef = MR32(q);
        if (ef && (MR8(ef+0x119u) & 1u)){
            c->esi = q;
            r = pl_call(c, ef, MR32(q+4u), 0x6605cdu);
            if ((int32_t)r < 0) goto done;
        }
    }
    if (!(MR8(th+0x119u) & 1u)){ r = 0; goto done; }
    r = pl_call(c, th, MR32(fp+8u), 0x6605eeu);
    }
done:
    c->eax = r;
    c->edi = sedi; c->esi = sesi; c->ebx = sebx; c->ebp = sebp; c->esp = fp + 4u;
    POP32(c->eip); c->esp += 4u;
}

/* SetMatrix's conversion step, sub_0065cf34(desc, target, source) ret 12.
 * Writes the 4x4 matrix TRANSPOSED, according to the parameter type ([desc+0]):
 *   3 float : target[4j+i] = source[4i+j] (bit copy)
 *   1 bool  : target[4j+i] = (source[4i+j] == [0x7d7d3c]) ? 0 : 1   (NaN -> 1)
 *   2 int   : via __ftol (0x75dd28) -- stays with the original
 *   other   : 0x80004005; stays with the original.
 * Success eax = 0.  If target/source overlap: the original (an in-place
 * transpose differs). */
static uint64_t sm_calls, sm_slow, smv_runs, smv_bad;
void sub_0065cf34_orig(CPU *c);
void sub_0065cf34(CPU *c){
    if (!dxn_on()){ sub_0065cf34_orig(c); return; }
    uint32_t sp = c->esp, t = MR32(MR32(sp+4u)), dst = MR32(sp+8u), src = MR32(sp+12u);
    sm_calls++;
    if ((t != 3u && t != 1u) || dst - src + 63u < 127u){ sm_slow++; sub_0065cf34_orig(c); return; }
    uint32_t S[16], D[16];
    memcpy(S, g_mem + src, 64);
    double z = (double)MRF32(0x7d7d3cu), last = 0;
    if (t == 3u){
        for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) D[4*j+i] = S[4*i+j];
    } else {
        for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++){
            float v; memcpy(&v, &S[4*i+j], 4); last = (double)v;
            D[4*j+i] = (last == z) ? 0u : 1u;
        }
    }
    if (g_dxn_v && (sm_calls % (uint64_t)g_dxn_v) == 0){
        sub_0065cf34_orig(c);
        smv_runs++;
        if ((memcmp(g_mem + dst, D, 64) || c->eax) && smv_bad++ < 8)
            fprintf(stderr, "[D3DXNVERIFY] SetMatrix conversion differs type %u target=%08x source=%08x eax=%08x\n",
                    t, dst, src, c->eax);
        return;
    }
    memcpy(g_mem + dst, D, 64);
    if (t == 3u){ c->ecx = src + 0x30u; }
    else { c->ecx = dst + 0x4cu; fcompare(c, z, last); }
    c->edx = 0; c->eax = 0;
    POP32(c->eip); c->esp += 12u;
}

/* Context of the pass being recorded: the state wrapper reports dependencies. */
#define PS_DEPS 48
typedef struct PassCtx { int elig; uint32_t tmask, ndep; uint32_t dep[PS_DEPS]; } PassCtx;
static PassCtx *g_pctx[8];
static int      g_pctx_n;
static inline void pctx_dep_range(uint32_t dp, uint32_t n){
    if (!g_pctx_n || !n) return;
    PassCtx *x = g_pctx[g_pctx_n-1];
    for (uint32_t l = dp >> 6; l <= (dp + n - 1u) >> 6; l++){
        uint32_t k = 0;
        for (; k < x->ndep; k++) if (x->dep[k] == l) break;
        if (k < x->ndep) continue;
        if (x->ndep >= PS_DEPS){ x->elig = 0; return; }
        x->dep[x->ndep++] = l;
    }
}
/* The parameter data a state reads: expression inputs + value buffer.
 * Called from both the state set (669286) and the value get (664a1f)
 * wrappers; the sub-states of sampler objects (type 14/15, 668dfd/668cc0)
 * do not go through 669286 but get their values via 664a1f. */
static void pctx_note_inputs(uint32_t R){
    if (MR32(R+0x38u) && !(MR8(R+0x30u) & 4) && MR32(R+0x3cu)){
        unsigned n = MR16(R+0x4eu); uint32_t base = MR32(R+0x34u);
        for (unsigned i = 0; i < n; i++){
            uint32_t dp, sz; slot_range(MR32(base + i*32u + 0xcu), &dp, &sz);
            pctx_dep_range(dp, sz);
        }
    }
    { uint32_t vb = MR32(R+8u); if (vb) pctx_dep_range(vb, 16u); }
}
static void pctx_note_state(uint32_t th, uint32_t P, uint32_t R, uint32_t type){
    if (!g_pctx_n) return;
    (void)th; (void)P;
    g_pctx[g_pctx_n-1]->tmask |= 1u << (type & 31u);
    if (type == 0 || type > 15){ g_pctx[g_pctx_n-1]->elig = 0; return; }
    pctx_note_inputs(R);
}

/* ---- Stage 2: state set memo, sub_00669286(this=ecx, a, b, elem) ret 0xc ----
 * Applies one state to the device.  If the snapshot of the inputs is the
 * same and ALL the D3D calls made on the last real run are now no-ops
 * (com.c dxrec_redundant), the function is skipped.
 *   D3DXMEMO2=0     : off (default on)
 *   D3DXVERIFY2=<n> : on 1/n of the predicted hits run the original with
 *                     recording; the memory window + "every call was a no-op"
 *                     are checked. */
#define M2_BUF 640u
typedef struct {
    uint32_t th, ab, elem, eax, n, len, type, valid;
    uint8_t  buf[M2_BUF];
    DxRec    rec;
    DxCall   rcall[8];
    uint8_t  rdata[256];
} M2Slot;
#define M2_CAP 8192u
static M2Slot *g_m2;
static int g_m2_on = -1, g_v2_n = 0;
static uint64_t m2_calls, m2_excl, m2_same, m2_hit, m2_skip, m2_norec, m2_devchg, m2_big;
static uint64_t v2_runs, v2_clean, v2_mem, v2_dev, v2_eax;
static uint64_t m2_type_calls[32], m2_type_hit[32], v2_type_bad[32];

static inline __attribute__((always_inline)) int snap_bytes(SnapW *w, uint32_t ga, uint32_t n){
    int ok = ga && mem_is_committed(ga) && mem_is_committed(ga + n - 1u);
    uint32_t tag = ok ? ga : 0u;
    if (!sw_put(w, &tag, 4)) return 0;
    return ok ? sw_put(w, g_mem + ga, n) : 1;
}
static inline __attribute__((always_inline)) int state_apply_snap(SnapW *w, uint32_t th, uint32_t a, uint32_t b, uint32_t el,
                            uint32_t P, uint32_t R){
    uint32_t fix[11] = { th, a, b, el, MR32(th+0x3cu), MR32(th+0x40u), MR32(th+0x44u),
                         MR32(th+0x118u) & ~1u, MR32(th+0xb0u), MR32(P), MR32(P+4u) };
    if (!sw_put(w, fix, sizeof fix)) return 0;
    if (!sw_put(w, g_mem + R, 0x64u)) return 0;
    if (!snap_bytes(w, MR32(P) + MR32(R+0x48u), 16u)) return 0;
    if (!snap_bytes(w, MR32(R+8u), 16u)) return 0;
    { uint32_t ot = MR32(th+0xb0u), ix = MR32(R+0x1cu);
      uint32_t oa = (ix < 0x10000u) ? ot + ix*4u : 0u;
      if (!snap_bytes(w, oa, 4u)) return 0; }
    if (MR32(R+0x38u) && !(MR8(R+0x30u) & 4))
        if (!state_input_snap(w, th, P, R)) return 0;
    return 1;
}

/* ---- Stage 4a: native state applier, sub_00669286 types 1/2/3 ----
 * An exact equivalent of the lifted code, on D3DX's own memory structures
 * (the bookkeeping is unchanged, every unsupported path falls back to the
 * original):
 *   P = a*0x64 + [this+0x2c], R = b*0x64 + [P+4], value = [[R+0x48] + [P]]
 *   if el == 0xffff then el = word[R+0x54]
 *   R+0 != 0 (dirty): R+0 = R+4; 664a1f(this, a, b) refreshes the value (<0: return)
 *   type = [R+0x58]>>24, state = [R+0x58]&0xffffff, device = [this+8]:
 *     1 SetRenderState(state, value)            vtable +0xe4
 *     2 SetTextureStageState(el, state, value)  +0x10c
 *     3 nothing if [this+0x3c]==1; otherwise SetSamplerState(el, state, value) +0x114
 *   Type 3 dirty + [this+0x3c]==4 + el==0xffff: a loop over every shader
 *   that uses the sampler -> original.  If the device call is a no-op (com.c
 *   dxn_skip) it is not made at all; otherwise it goes to com_call through
 *   the guest dispatch.
 *   D3DXNATIVE4=0     : off
 *   D3DXN4VERIFY=<n>  : on 1/n of the eligible calls the native code does a
 *                       dry run, memory + call list are saved, the original
 *                       runs with recording, and the two are compared. */
int dxn_skip(int kind, uint32_t a0, uint32_t a1, uint32_t a2);
void sub_00664a1f(CPU *c);
void sub_00669286_orig(CPU *c);
void dispatch_indirect(CPU*, uint32_t);
static int g_n4_on = -1, g_n4_v = 0;
static uint64_t n4_calls, n4_dirty, n4_issued, n4_skipped;
#define NDRY 64
typedef struct { uint64_t runs, bad, norec; } VStat;
#define VEXT 40
static void nat_verify(CPU *c, int (*nat)(CPU*), void (*orig)(CPU*), uint32_t th,
                       const uint32_t *ext, int next, VStat *vs, const char *tag, uint32_t info);
static int n4_dry, n4_dryn; static uint32_t n4_dryk[NDRY][4];
/* kind: 1 SetRenderState(a0,a1) 2 SetTextureStageState 3 SetSamplerState
 *       (a0,a1,a2) 4 SetTexture(a0,a1) 5/6 Set{V,P}SConstantF(start, data,
 *       count) 7/8 Set{Vertex,Pixel}Shader(a0) 9/10 ...ConstantI 11/12 ...ConstantB
 *       (9..12 have no no-op check: always made) */
static const int n4_nargs[13] = { 0, 2, 3, 3, 2, 3, 3, 1, 1, 3, 3, 3, 3 };
/* Inline equivalent of com.c dxrec_call_redundant for types 1..3 (same rule);
 * -1 while recording/replay/trace is on: the slow path (dxn_skip) decides. */
extern uint32_t g_rs[256], g_tss_raw[8][40], g_smp_raw[16][20];
extern int g_rs_dirty, g_sb_rec, g_sb_replay, g_dxrec_depth, g_fxtr;
static inline int n4_red_fast(int kind, uint32_t a0, uint32_t a1, uint32_t a2){
    if (g_sb_rec | g_sb_replay | g_dxrec_depth | g_fxtr) return -1;
    switch (kind){
    case 1: return a0 >= 256u || (g_rs[a0] == a1 && !g_rs_dirty);
    case 2: return !(a0 < 8u && a1 < 40u) || g_tss_raw[a0][a1] == a2;
    case 3: return !(a0 < 16u && a1 < 20u) || g_smp_raw[a0][a1] == a2;
    }
    return -1;
}
/* Stage 9 (n5_pass): the pass application being recorded */
#define PR_CALLS 48
typedef struct { uint8_t kind; uint32_t a0, a1, a2; } PrCall;
typedef struct { uint32_t th, a, el, mode, epoch, n; uint8_t ok, bad; PrCall c[PR_CALLS]; } PassRep;
static PassRep *g_pr_cur;
static inline uint32_t n4_dev(CPU *c, uint32_t dev, int kind, uint32_t off, uint32_t ret,
                              uint32_t a0, uint32_t a1, uint32_t a2){
    if (g_pr_cur){
        PassRep *p = g_pr_cur;
        if (p->n < PR_CALLS){ PrCall *k = &p->c[p->n++]; k->kind = (uint8_t)kind; k->a0 = a0; k->a1 = a1; k->a2 = a2; }
        else p->bad = 1;
    }
    if (n4_dry){
        if (n4_dryn < NDRY){ n4_dryk[n4_dryn][0] = (uint32_t)kind; n4_dryk[n4_dryn][1] = a0;
                             n4_dryk[n4_dryn][2] = a1; n4_dryk[n4_dryn][3] = a2; }
        n4_dryn++; return 0;
    }
    int f = n4_red_fast(kind, a0, a1, a2);
    if (f > 0 || (f < 0 && dxn_skip(kind, a0, a1, a2))){ n4_skipped++; return 0; }
    n4_issued++;
    uint32_t fn = MR32(MR32(dev) + off);
    int na = n4_nargs[kind];
    if (na >= 3) PUSH32(a2);
    if (na >= 2) PUSH32(a1);
    PUSH32(a0); PUSH32(dev); PUSH32(ret);
    dispatch_indirect(c, fn);
    return c->eax;
}
/* Per-state memo for a dirty expression state (Stage 1's logic, small slots
 * indexed directly by R): if the input parameters' data, the target dword
 * and the first dword of the value buffer did not change since the last
 * real 664a1f, the evaluation would write the same result -> skip.  At most
 * 2 inputs / 64 bytes; larger ones are always evaluated. */
typedef struct { uint32_t R, th, dst, dval, vb, vval, eax, n, base; uint32_t rec[2], pp[2], dp[2], nb[2]; uint8_t d[96]; } N4Memo;
#define N4M_CAP 2048u
static N4Memo *g_n4m;
static int g_n4m_on = -1;
static uint64_t n4m_hit, n4m_miss, n4m_big, n4m_why[4];
void sub_00664a1f_orig(CPU *c);
/* 1: inputs gathered (rec, pp, dp, nb filled); 0: cannot be memoized */
static inline int n4m_inputs(uint32_t R, uint32_t *n, uint32_t *rec, uint32_t *pp, uint32_t *dp, uint32_t *nb){
    if (!MR32(R+0x38u) || (MR8(R+0x30u) & 4)){ n4m_why[0]++; return 0; }
    if (!MR32(R+0x3cu)){ n4m_why[1]++; return 0; }
    uint32_t k = MR16(R+0x4eu), base = MR32(R+0x34u), tot = 0;
    if (k == 0 || k > 2u){ n4m_why[2]++; return 0; }
    for (uint32_t i = 0; i < k; i++){
        uint32_t r = base + i*32u, p = MR32(r + 0xcu), d = MR32(p), ds = MR32(p+4u);
        uint32_t el = MR32(ds+0x10u); if (!el) el = 1;
        uint32_t rows = MR32(ds+0x14u); if (!rows) rows = 1;
        uint32_t b = d ? el*rows*16u : 0u;
        tot += b; if (tot > 96u){ n4m_why[3]++; return 0; }
        rec[i] = r; pp[i] = p; dp[i] = d; nb[i] = b;
    }
    *n = k; return 1;
}
static inline uint32_t n4_eval(CPU *c, uint32_t th, uint32_t a, uint32_t b, uint32_t R, uint32_t P, uint32_t ret){
    if (g_n4m_on < 0){ const char *e = getenv("D3DXN4MEMO"); g_n4m_on = e ? atoi(e) != 0 : 1;
                       if (g_n4m_on) g_n4m = calloc(N4M_CAP, sizeof *g_n4m); }
    N4Memo *s = NULL;
    uint32_t dst = MR32(R+0x48u) + MR32(P), vb = MR32(R+8u);
    if (g_n4m){
        s = &g_n4m[((R >> 2) * 0x9E3779B1u) >> 21];
        /* hit: same state, the input records point at the same parameter,
         * data and target/value buffer as at the last real evaluation */
        if (s->R == R && s->th == th && s->dst == dst && s->vb == vb && s->n
            && MR16(R+0x4eu) == s->n && MR32(R+0x34u) == s->base
            && MR32(s->rec[0] + 0xcu) == s->pp[0] && MR32(s->pp[0]) == s->dp[0]
            && (s->n < 2 || (MR32(s->rec[1] + 0xcu) == s->pp[1] && MR32(s->pp[1]) == s->dp[1]))
            && rd32s(dst) == s->dval && rd32s(vb) == s->vval
            && !memcmp(s->d, g_mem + s->dp[0], s->nb[0])
            && (s->n < 2 || !memcmp(s->d + s->nb[0], g_mem + s->dp[1], s->nb[1]))){
            n4m_hit++;
            return s->eax;
        }
    }
    uint32_t n = 0, rec[2] = {0,0}, pp[2] = {0,0}, dp[2] = {0,0}, nb[2] = {0,0};
    int ok = g_n4m && n4m_inputs(R, &n, rec, pp, dp, nb);
    if (g_n4m){ if (ok) n4m_miss++; else n4m_big++; }
    c->ecx = th; PUSH32(b); PUSH32(a); PUSH32(ret);
    if (g_n4m) sub_00664a1f_orig(c); else sub_00664a1f(c);
    uint32_t r = c->eax;
    if (ok){
        s->R = R; s->th = th; s->dst = dst; s->vb = vb; s->n = n; s->eax = r; s->base = MR32(R+0x34u);
        for (uint32_t i = 0; i < 2; i++){ s->rec[i] = rec[i]; s->pp[i] = pp[i]; s->dp[i] = dp[i]; s->nb[i] = nb[i]; }
        memcpy(s->d, g_mem + dp[0], nb[0]); if (n > 1) memcpy(s->d + nb[0], g_mem + dp[1], nb[1]);
        s->dval = rd32s(dst); s->vval = rd32s(vb);
    } else if (s) s->R = 0;
    return r;
}
/* ---- Stage 4c: native sampler applier, sub_00668cc0(this, P, Rbegin, Rend) ret 12 ----
 * For every sampler binding state R in [Rbegin, Rend):
 *   vp = [R+0x48] + [P]; record = w[vp+2] + w[vp] - 1, w[R+0x56] consecutive records,
 *   stage = w[R+0x54] (+1 per record).  The states of each record (P2):
 *     type 3: if [R2] is dirty 664a1f(this, record no, k), then [R2] = [R2+4];
 *             SetSamplerState(stage, [R2+0x58]&0xffffff, [[R2+0x48]+[P2]])
 *     type 9: o = [[this+0xb0] + [R2+0x1c]*4], i = [[R2+8]],
 *             if [o+0x40] then i = [[o+0x40]+8][i]; SetTexture(stage, [[this+0x18] + i*24 + 4])
 *   On a <0 result stop and return it; 0 at the end.  A record with any
 *   other type -> original. */
static uint64_t n6_calls;
static VStat n6vs;
static int g_n6_on = -1;
static int n6_samp(CPU *c, uint32_t th, uint32_t P, uint32_t Rb, uint32_t Re, uint32_t *res){
    uint32_t base = MR32(th+0x2cu);
    for (uint32_t R = Rb; R < Re; R += 0x64u){
        uint32_t vp = MR32(R+0x48u) + MR32(P);
        uint32_t P2 = (MR16(vp+2u) + MR16(vp))*0x64u + base - 0x64u, end = MR16(R+0x56u)*0x64u + P2;
        for (; P2 < end; P2 += 0x64u){
            uint32_t R2 = MR32(P2+4u);
            for (uint32_t k = 0; k < MR16(P2+0x36u); k++, R2 += 0x64u){
                uint32_t t = MR8(R2+0x5bu);
                if (t != 3u && t != 9u) return 0;
            }
        }
    }
    n6_calls++;
    uint32_t dev = MR32(th+8u), r;
    for (uint32_t R = Rb; R < Re; R += 0x64u){
        uint32_t vp = MR32(R+0x48u) + MR32(P), stage = MR16(R+0x54u);
        uint32_t P2 = (MR16(vp+2u) + MR16(vp))*0x64u + base - 0x64u, end = MR16(R+0x56u)*0x64u + P2;
        for (; P2 < end; P2 += 0x64u, stage++){
            uint32_t R2 = MR32(P2+4u);
            for (uint32_t k = 0; k < MR16(P2+0x36u); k++, R2 += 0x64u){
                if (MR8(R2+0x5bu) == 3u){
                    if (MR32(R2)){
                        uint32_t rec = (uint32_t)((int32_t)(P2 - MR32(th+0x2cu)) / 0x64) & 0xffffu;
                        r = n4_eval(c, th, rec, k, R2, P2, 0x668d41u);
                        if ((int32_t)r < 0){ *res = r; return 1; }
                        MR32(R2) = MR32(R2+4u);
                    }
                    r = n4_dev(c, dev, 3, 0x114u, 0x668d6du, stage, MR32(R2+0x58u) & 0xffffffu,
                               MR32(MR32(R2+0x48u) + MR32(P2)));
                } else {
                    uint32_t o = MR32(MR32(th+0xb0u) + MR32(R2+0x1cu)*4u), o40 = MR32(o+0x40u);
                    uint32_t ix = MR32(MR32(R2+8u));
                    if (o40) ix = MR32(MR32(o40+8u) + ix*4u);
                    r = n4_dev(c, dev, 4, 0x104u, 0x668db4u, stage, MR32(MR32(th+0x18u) + ix*24u + 4u), 0);
                }
                if ((int32_t)r < 0){ *res = r; return 1; }
            }
        }
    }
    *res = 0;
    return 1;
}
static int n6_apply(CPU *c){
    uint32_t sp = c->esp, res;
    if (!n6_samp(c, c->ecx, MR32(sp+4u), MR32(sp+8u), MR32(sp+12u), &res)) return 0;
    c->eax = res; POP32(c->eip); c->esp += 12u;
    return 1;
}
void sub_00668cc0_orig(CPU *c);
void sub_00668cc0(CPU *c){
    if (g_n6_on < 0){ const char *e = getenv("D3DXNATIVE6"); g_n6_on = e ? atoi(e) != 0 : 1; }
    if (g_n6_on && !g_pctx_n){
        if (g_n4_v){
            static uint64_t vk;
            if ((++vk % (uint64_t)g_n4_v) == 0){
                uint32_t ext[VEXT], ne = 0, sp = c->esp, th = c->ecx, P = MR32(sp+4u), base = MR32(th+0x2cu);
                ext[ne++] = P;
                for (uint32_t R = MR32(sp+8u); R < MR32(sp+12u) && ne < VEXT; R += 0x64u){
                    ext[ne++] = R;
                    uint32_t vp = MR32(R+0x48u) + MR32(P);
                    uint32_t P2 = (MR16(vp+2u) + MR16(vp))*0x64u + base - 0x64u, end = MR16(R+0x56u)*0x64u + P2;
                    for (; P2 < end && ne + 1 < VEXT; P2 += 0x64u){
                        ext[ne++] = P2;
                        for (uint32_t k = 0, R2 = MR32(P2+4u); k < MR16(P2+0x36u) && ne < VEXT; k++, R2 += 0x64u) ext[ne++] = R2;
                    }
                }
                nat_verify(c, n6_apply, sub_00668cc0_orig, th, ext, (int)ne, &n6vs, "D3DXN6VERIFY", P);
                return;
            }
        }
        if (n6_apply(c)) return;
    }
    sub_00668cc0_orig(c);
}

/* ---- Stage 4d: native shader applier, sub_00668dfd(this, P) ret 4 ----
 * P = shader record; R0 = [P+4] the shader object state, vs = [P+8] bit 1.
 *   If R0 is dirty: 669286(this, record, 0, 0xffff) -> original.
 *   Set{Vertex,Pixel}Shader(from the object table; o+0x40 indirect index)
 *   P&4: 663bb1(this, record, 1); if P&0x10 upload word[P+0x50/56/5c] F/I/B
 *        constants (if 4c==4e / 52==54 / 58==5a); skip one state.
 *   float constant states [.., w[P+0x4e]): if dirty R=R+4, and if R+0x30&0x10
 *        a raw copy ([R+0x5c], [R+0x60] bytes), otherwise 6623da; then upload
 *        the block (no upload if the loop was empty).  If w[P+0x4e]==w[P+0x36]
 *        return 0.
 *   int [.., w[P+0x54]) -> 662795 + ConstantI; bool [.., w[P+0x5a]) ->
 *        662b9b + ConstantB; the remaining [.., w[P+0x60]) samplers -> 668cc0.
 * The local slot [ebp-8] is passed by address to the filler functions; it is
 * emulated with the same update rule.  In a dry verification run, if a guest
 * sub-call is needed the native path is refused (the original runs). */
void sub_00663bb1(CPU*); void sub_006623da(CPU*); void sub_00662795(CPU*); void sub_00662b9b(CPU*);
void sub_00668dfd_orig(CPU *c);
static uint64_t n7_calls;
static VStat n7vs;
static int g_n7_on = -1;
/* filler: fn(this, R+8, target, w[R+0x56], [R+0x3c], [[R+0x34]+0x10], &slot, [o+0x4c]) */
static inline void n7_fill(CPU *c, void (*fn)(CPU*), uint32_t ret, uint32_t th, uint32_t P, uint32_t R, uint32_t *slot8){
    uint32_t o = MR32(MR32(th+0xb0u) + MR32(R+0x1cu)*4u);
    PUSH32(*slot8); uint32_t sa = c->esp;
    PUSH32(MR32(o+0x4cu)); PUSH32(sa); PUSH32(MR32(MR32(R+0x34u)+0x10u)); PUSH32(MR32(R+0x3cu));
    PUSH32(MR16(R+0x56u)); PUSH32(MR32(R+0x48u) + MR32(P)); PUSH32(R+8u); PUSH32(ret);
    c->ecx = th; fn(c);
    *slot8 = MR32(sa); c->esp = sa + 4u;
}
static int n7_shader(CPU *c, uint32_t th, uint32_t P, uint32_t *res){
    uint32_t R0 = MR32(P+4u), vs = (MR32(P+8u) & 2u) != 0, base = MR32(th+0x2cu);
    if (MR32(R0)) return 0;
    uint32_t endf = MR16(P+0x4eu)*0x64u + MR32(P+4u), ends = MR16(P+0x60u)*0x64u + MR32(P+4u);
    if (n4_dry){
        if (MR8(P+8u) & 4u) return 0;
        for (uint32_t R = R0 + 0x64u; R < endf; R += 0x64u)
            if (MR32(R) && !(MR8(R+0x30u) & 0x10u)) return 0;
        if (MR16(P+0x4eu) != MR16(P+0x36u) && endf < MR16(P+0x5au)*0x64u + MR32(P+4u)) return 0;
    }
    n7_calls++;
    uint32_t dev = MR32(th+8u), r, slot8;
    uint32_t o = MR32(MR32(th+0xb0u) + MR32(R0+0x1cu)*4u), o40 = MR32(o+0x40u), ix = MR32(MR32(R0+8u));
    if (o40) ix = MR32(MR32(o40+8u) + ix*4u);
    uint32_t sh = MR32(MR32(th+0x18u) + ix*24u + 4u);
    r = vs ? n4_dev(c, dev, 7, 0x170u, 0x668e69u, sh, 0, 0) : n4_dev(c, dev, 8, 0x1acu, 0x668e99u, sh, 0, 0);
    if ((int32_t)r < 0){ *res = r; return 1; }
    uint32_t R = R0 + 0x64u; slot8 = R;
    if (MR8(P+8u) & 4u){
        uint32_t rec = (uint32_t)((int32_t)(P - base) / 0x64) & 0xffffu;
        c->ecx = th; PUSH32(1u); PUSH32(rec); PUSH32(0x668ec9u); sub_00663bb1(c);
        r = c->eax; if ((int32_t)r < 0){ *res = r; return 1; }
        if (MR8(P+8u) & 0x10u){
            uint32_t n;
            if ((n = MR16(P+0x50u)) && MR16(P+0x4cu) == MR16(P+0x4eu))
                n4_dev(c, dev, vs ? 5 : 6, vs ? 0x178u : 0x1b4u, vs ? 0x668f0eu : 0x668f16u, 0, MR32(P+0x40u)+MR32(P), n);
            if ((n = MR16(P+0x56u)) && MR16(P+0x52u) == MR16(P+0x54u))
                n4_dev(c, dev, vs ? 9 : 10, vs ? 0x180u : 0x1bcu, vs ? 0x668f49u : 0x668f51u, 0, MR32(P+0x44u)+MR32(P), n);
            if ((n = MR16(P+0x5cu)) && MR16(P+0x58u) == MR16(P+0x5au))
                n4_dev(c, dev, vs ? 11 : 12, vs ? 0x188u : 0x1c4u, vs ? 0x668f84u : 0x668f8cu, 0, MR32(P+0x48u)+MR32(P), n);
        }
        R += 0x64u; slot8 = R;
    }
    if (R < endf){
        for (; R < endf; R += 0x64u, slot8 = R){
            if (!MR32(R)) continue;
            MR32(R) = MR32(R+4u);
            if (MR8(R+0x30u) & 0x10u){
                uint32_t dst = MR32(R+0x48u) + MR32(P), src = MR32(R+0x5cu), nb = MR32(R+0x60u);
                if (dst + nb <= src || src + nb <= dst) memcpy(g_mem + dst, g_mem + src, nb);
                else { for (uint32_t k = 0; k + 4u <= nb; k += 4u) MR32(dst+k) = MR32(src+k);   /* rep movsd */
                       for (uint32_t k = nb & ~3u; k < nb; k++) MR8(dst+k) = MR8(src+k); }
            } else n7_fill(c, sub_006623da, 0x669007u, th, P, R, &slot8);
        }
        r = n4_dev(c, dev, vs ? 5 : 6, vs ? 0x178u : 0x1b4u, vs ? 0x669035u : 0x66903du,
                   0, MR32(P+0x40u)+MR32(P), MR16(P+0x50u));
        if ((int32_t)r < 0){ *res = r; return 1; }
    }
    if (MR16(P+0x4eu) == MR16(P+0x36u)){ *res = 0; return 1; }
    uint32_t endi = MR16(P+0x54u)*0x64u + MR32(P+4u);
    if (R < endi){
        for (; R < endi; R += 0x64u)
            if (MR32(R)){ MR32(R) = MR32(R+4u); n7_fill(c, sub_00662795, 0x6690a3u, th, P, R, &slot8); }
        r = n4_dev(c, dev, vs ? 9 : 10, vs ? 0x180u : 0x1bcu, vs ? 0x6690ceu : 0x6690d6u,
                   0, MR32(P+0x44u)+MR32(P), MR16(P+0x56u));
        if ((int32_t)r < 0){ *res = r; return 1; }
    }
    uint32_t endb = MR16(P+0x5au)*0x64u + MR32(P+4u);
    if (R < endb){
        for (; R < endb; R += 0x64u)
            if (MR32(R)){ MR32(R) = MR32(R+4u); n7_fill(c, sub_00662b9b, 0x66912bu, th, P, R, &slot8); }
        r = n4_dev(c, dev, vs ? 11 : 12, vs ? 0x188u : 0x1c4u, vs ? 0x669156u : 0x66915eu,
                   0, MR32(P+0x48u)+MR32(P), MR16(P+0x5cu));
        if ((int32_t)r < 0){ *res = r; return 1; }
    }
    if (R >= ends){ *res = 0; return 1; }
    if (!n6_samp(c, th, P, R, ends, &r)){
        c->ecx = th; PUSH32(ends); PUSH32(R); PUSH32(P); PUSH32(0x66917eu); sub_00668cc0_orig(c);
        r = c->eax;
    }
    *res = r;
    return 1;
}
static int n7_apply(CPU *c){
    uint32_t res;
    if (!n7_shader(c, c->ecx, MR32(c->esp+4u), &res)) return 0;
    c->eax = res; POP32(c->eip); c->esp += 4u;
    return 1;
}
void sub_00668dfd(CPU *c){
    if (g_n7_on < 0){ const char *e = getenv("D3DXNATIVE7"); g_n7_on = e ? atoi(e) != 0 : 1; }
    uint64_t t0 = dx_on() ? mach_absolute_time() : 0;
    if (g_n7_on && !g_pctx_n){
        static uint64_t vk;
        if (g_n4_v && (++vk % (uint64_t)g_n4_v) == 0){
            uint32_t th = c->ecx, P = MR32(c->esp+4u), ext[VEXT], ne = 0;
            ext[ne++] = P;
            uint32_t ends = MR16(P+0x60u)*0x64u + MR32(P+4u);
            for (uint32_t R = MR32(P+4u); R < ends && ne < VEXT - 8; R += 0x64u) ext[ne++] = R;
            uint32_t blk = MR32(P+0x40u) + MR32(P), nb = MR16(P+0x50u)*16u;
            for (uint32_t k = 0; k < nb && ne < VEXT; k += 0x64u) ext[ne++] = blk + k;
            nat_verify(c, n7_apply, sub_00668dfd_orig, th, ext, (int)ne, &n7vs, "D3DXN7VERIFY", P);
            goto out;
        }
        if (n7_apply(c)) goto out;
    }
    sub_00668dfd_orig(c);
out:
    if (t0){ g_dx[DX_668E0D].ticks += mach_absolute_time() - t0; g_dx[DX_668E0D].n++; }
}

/* ---- Stage 4e: native SetTexture path ----
 * sub_0065ccc8(this, tex, i, o40) ret 12: object table slot
 *   y = [this+0x18] + (o40 ? [[o40+8] + i*4] : i)*24;  0 if [y+4] == tex.
 *   tex->AddRef, old->Release, [y+4] = tex; if o40, the same for each
 *   effect's slot in the pool ring ([o40+0x10] .. o40, node: +0 effect,
 *   +8 index table): AddRef(new), Release(old), store.  Returns 0.
 * In com.c texture AddRef/Release has no side effect at all (returns 1/0):
 *   if the target is a com.c texture the call is skipped.
 * sub_0066d7d9 SetTexture(this, h, tex) ret 12: if h < 0 then record = ~h
 *   (a string handle -> original); an array parameter without an element, or
 *   a type outside 5..9 -> 0x8876086c; [this+0x3c]==3 -> original (the 666a78
 *   path); 0 if the slot is unchanged; otherwise 65ccc8 then 66055c(this, record).
 *   D3DXNATIVE8=0 turns it off. */
void sub_0065ccc8_orig(CPU *c);
extern int g_fxtr; int fxtr_now(void);
static int g_n8_on = -1;
static uint64_t n8_tex, n8_set, n8_refskip;
static inline int n8_init(void){
    if (g_n8_on < 0){ const char *e = getenv("D3DXNATIVE8"); g_n8_on = e ? atoi(e) != 0 : 1; }
    return g_n8_on;
}
/* v12na: com.c textures are reference counted now (and freed on the last
 * Release), so the effect's references must be counted: skipping them let a
 * texture shared by two effects be freed by the first effect's
 * OnLostDevice and released again by the second (NFSU2_HEAPCHECK, options
 * Reset: write after free at 0x65c88d).  The count is still updated
 * directly, without the guest dispatch; NFSU2_COMFREE=0 restores the skip. */
uint32_t com_obj_addref(CPU *c, uint32_t o); uint32_t com_obj_release(CPU *c, uint32_t o); int com_frees(void);
void com_obj_refn(CPU *c, uint32_t o, int32_t n);
static inline int n8_frees(void){ static int v = -1; if (v < 0) v = com_frees(); return v; }
/* AddRef / Release of obj (vtable +4 / +8) are com.c's texture methods */
static inline int n8_com(uint32_t obj, uint32_t off){
    uint32_t fn = MR32(MR32(obj) + off);
    return (fn & 0xF0000000u) == 0xE0000000u && ((fn >> 16) & 0x1fffu) == 3u;
}
static inline void n8_ref(CPU *c, uint32_t obj, uint32_t off, uint32_t ret){
    if (n8_com(obj, off) && !(g_fxtr && fxtr_now())){
        n8_refskip++;
        if (n8_frees()){ if (off == 4u) com_obj_addref(c, obj); else com_obj_release(c, obj); }
        return; }
    PUSH32(obj); PUSH32(ret); dispatch_indirect(c, MR32(MR32(obj) + off));
}
/* The pool ring of n8_store when every reference in it is a com.c texture and
 * no node is the slot y itself: the same slot stores and reference counts as
 * the loop in n8_store, with the AddRefs of the new texture made first and
 * the Releases of equal old textures in one call each (a ring of ~13 effects
 * made ~27 calls into com.c per SetTexture, ~2,700 a race frame).  Neither
 * runs guest code, so the order only moves when a texture's last Release
 * frees it, which no other step of the loop reads.  0: not applicable. */
static int n8_ring(CPU *c, uint32_t y, uint32_t ix, uint32_t o40){
    if (g_fxtr && fxtr_now()) return 0;
    uint32_t nw = MR32(y+4u), k = 0;
    if (nw && !(n8_com(nw, 4u) && n8_com(nw, 8u))) return 0;
    for (uint32_t nd = MR32(o40+0x10u); nd != o40; nd = MR32(nd+0x10u)){
        uint32_t ef = MR32(nd);
        if (!ef) continue;
        uint32_t y2 = MR32(ef+0x18u) + MR32(MR32(nd+8u) + ix*4u)*24u, o2 = MR32(y2+4u);
        if (y2 == y || (o2 && !(n8_com(o2, 4u) && n8_com(o2, 8u)))) return 0;
        k++;
    }
    int fr = n8_frees();
    if (nw && k){ n8_refskip += k; if (fr) com_obj_refn(c, nw, (int32_t)k); }
    uint32_t run = 0; int32_t rn = 0;
    for (uint32_t nd = MR32(o40+0x10u); nd != o40; nd = MR32(nd+0x10u)){
        uint32_t ef = MR32(nd);
        if (!ef) continue;
        uint32_t y2 = MR32(ef+0x18u) + MR32(MR32(nd+8u) + ix*4u)*24u, o2 = MR32(y2+4u);
        if (o2){
            n8_refskip++;
            if (o2 != run){ if (rn && fr) com_obj_refn(c, run, -rn); run = o2; rn = 0; }
            rn++;
        }
        MR32(y2+4u) = nw;
    }
    if (rn && fr) com_obj_refn(c, run, -rn);
    return 1;
}
static void n8_store(CPU *c, uint32_t th, uint32_t tex, uint32_t ix, uint32_t o40){
    n8_tex++;
    uint32_t y = MR32(th+0x18u) + (o40 ? MR32(MR32(o40+8u) + ix*4u) : ix)*24u;
    if (MR32(y+4u) == tex) return;
    if (tex) n8_ref(c, tex, 4u, 0x65cd00u);
    uint32_t old = MR32(y+4u);
    if (old){ n8_ref(c, old, 8u, 0x65cd0du); MR32(y+4u) = 0; }
    MR32(y+4u) = tex;
    if (!o40 || n8_ring(c, y, ix, o40)) return;
    for (uint32_t nd = MR32(o40+0x10u); nd != o40; nd = MR32(nd+0x10u)){
        uint32_t ef = MR32(nd);
        if (!ef) continue;
        uint32_t y2 = MR32(ef+0x18u) + MR32(MR32(nd+8u) + ix*4u)*24u, nw = MR32(y+4u);
        if (nw) n8_ref(c, nw, 4u, 0x65cd42u);
        uint32_t o2 = MR32(y2+4u);
        if (o2){ n8_ref(c, o2, 8u, 0x65cd4fu); MR32(y2+4u) = 0; }
        MR32(y2+4u) = MR32(y+4u);
    }
}
void sub_0065ccc8(CPU *c){
    if (!n8_init()){ sub_0065ccc8_orig(c); return; }
    uint32_t sp = c->esp, ret = MR32(sp), th = c->ecx;
    PUSH32(c->ebp); PUSH32(c->ebx); PUSH32(c->esi); PUSH32(c->edi);  /* original prologue: sub-calls must see the same esp */
    n8_store(c, th, MR32(sp+4u), MR32(sp+8u), MR32(sp+12u));
    c->esp = sp + 16u; c->eip = ret; c->eax = 0;
}
void sub_0066055c(CPU *c);
void sub_0066d7d9_orig(CPU *c);
void sub_0066d7d9(CPU *c){
    uint64_t t0 = dx_on() ? mach_absolute_time() : 0;
    uint32_t sp = c->esp, th = MR32(sp+4u), h = MR32(sp+8u), tex = MR32(sp+12u);
    if (!n8_init() || (int32_t)h >= 0 || MR32(th+0x3cu) == 3u){ g_fx_epoch++; sub_0066d7d9_orig(c); goto out; }
    {
    n8_set++;
    uint32_t ret = MR32(sp), rec = ~h, d = MR32(rec+4u), r;
    if ((MR32(d+0x10u) && MR32(rec+8u) == 0xffffffffu) || (int32_t)MR32(d) < 5 || (int32_t)MR32(d) > 9){
        r = 0x8876086cu; goto done;
    }
    uint32_t o40 = MR32(MR32(MR32(th+0xb0u) + MR32(rec+0x14u)*4u) + 0x40u), ix = MR32(MR32(rec));
    uint32_t e = o40 ? MR32(MR32(o40+8u) + ix*4u) : ix;
    if (tex == MR32(MR32(th+0x18u) + e*24u + 4u)){ r = 0; goto done; }
    g_fx_epoch++;                                        /* stage 9: the texture changes */
    PUSH32(c->ebp); PUSH32(c->ebx); PUSH32(c->esi); PUSH32(c->edi);
    PUSH32(c->ebp); PUSH32(c->ebx); PUSH32(c->esi); PUSH32(c->edi);   /* 65ccc8's prologue */
    n8_store(c, th, tex, ix, o40);
    c->esp += 16u;
    c->ecx = th; PUSH32(rec); PUSH32(0x66d89fu); sub_0066055c(c);
    r = c->eax;
    c->esp += 16u;
done:
    c->eax = r; c->esp = sp + 16u; c->eip = ret;
    }
out:
    if (t0){ g_dx[DX_SETTEXTURE].ticks += mach_absolute_time() - t0; g_dx[DX_SETTEXTURE].n++; }
}

/* Core: if it returns 1 the state was applied natively, *res = 669286's
 * return value.  Does not touch the stack (except push/pop for 664a1f and
 * the device calls). */
void sub_006691a1(CPU *c); void sub_00668dfd(CPU*);
static uint64_t n4_obj, n4_tex;
static inline int n4_core(CPU *c, uint32_t th, uint32_t a, uint32_t b, uint32_t eld, uint32_t *res){
    uint32_t P = a*0x64u + MR32(th+0x2cu), R = b*0x64u + MR32(P+4u);
    uint32_t code = MR32(R+0x58u), type = code >> 24, el = eld & 0xffffu;
    if (el == 0xffffu) el = MR16(R+0x54u);
    int dirty = MR32(R) != 0;
    if (type == 14u || type == 15u){
        /* object assignment (14 shader, 15 sampler): dirty + array selector -> original */
        if (dirty && MR32(R+0x38u)) return 0;
        n4_obj++;
        if (dirty) MR32(R) = MR32(R+4u);
        uint32_t vp = MR32(R+0x48u) + MR32(P), cnt = MR16(vp), r = 0;
        if (!cnt){ *res = 0; return 1; }
        uint32_t rec = MR16(vp+2u) + cnt - 1u, mode = MR32(th+0x3cu);
        if (type == 14u){
            if (mode == 4u){
                uint32_t P2 = rec*0x64u + MR32(th+0x2cu);
                c->ecx = th;
                if (MR8(P2+8u) & 1u){ PUSH32(P2); PUSH32(0x669f63u); sub_00668dfd(c); }
                else { PUSH32(0xffffu); PUSH32(rec); PUSH32(0x669f75u); sub_006691a1(c); }
                *res = 0; return 1;
            }
            c->ecx = th; PUSH32(0xffffu); PUSH32(rec); PUSH32(0x669f96u); sub_006691a1(c);
            *res = c->eax; return 1;
        }
        if (mode == 4u){
            if (!n6_samp(c, th, P, R, R + 0x64u, &r)){
                c->ecx = th; PUSH32(R + 0x64u); PUSH32(R); PUSH32(P); PUSH32(0x669fd5u); sub_00668cc0_orig(c);
                r = c->eax;
            }
            *res = r; return 1;
        }
        c->ecx = th; PUSH32((eld & 0xffff0000u) | el); PUSH32(rec); PUSH32(0x6693b1u); sub_006691a1(c);
        *res = c->eax; return 1;
    }
    if (type == 9u){
        /* a texture: SetTexture(el, the effect's object table entry for the
         * parameter), as 669286's 0x669B51.  Mode 2 asks the state manager
         * first, and a dirty one in mode 4 with no element loops over the
         * shaders that use it (0x66941A): those stay with the original. */
        uint32_t mode = MR32(th+0x3cu);
        if (mode == 2u || (dirty && mode == 4u && el == 0xffffu)) return 0;
        n4_tex++;
        if (dirty) MR32(R) = MR32(R+4u);
        uint32_t o = MR32(MR32(th+0xb0u) + MR32(R+0x1cu)*4u), o40 = MR32(o+0x40u), ix = MR32(MR32(R+8u));
        if (o40) ix = MR32(MR32(o40+8u) + ix*4u);
        *res = n4_dev(c, MR32(th+8u), 4, 0x104u, 0x669b8du, el, MR32(MR32(th+0x18u) + ix*24u + 4u), 0);
        return 1;
    }
    if (type - 1u > 2u) return 0;
    if (dirty && type == 3u && MR32(th+0x3cu) == 4u && el == 0xffffu) return 0;
    n4_calls++;
    uint32_t vp = MR32(R+0x48u) + MR32(P), r = 0;
    if (dirty){
        n4_dirty++;
        MR32(R) = MR32(R+4u);
        r = n4_eval(c, th, a, b, R, P, type == 3u ? 0x669523u : 0x669853u);
        if ((int32_t)r < 0){ *res = r; return 1; }
    }
    uint32_t dev = MR32(th+8u), v = MR32(vp), st = code & 0xffffffu;
    if (type == 1u)      r = n4_dev(c, dev, 1, 0xe4u,  0x66988eu, st, v, 0);
    else if (type == 2u) r = n4_dev(c, dev, 2, 0x10cu, 0x6698afu, el, st, v);
    else if (MR32(th+0x3cu) != 1u) r = n4_dev(c, dev, 3, 0x114u, 0x6698dau, el, st, v);
    *res = r;
    return 1;
}
static int n4_apply(CPU *c){
    uint32_t th = c->ecx, sp = c->esp, res;
    uint32_t a = MR16(sp+4u), b = MR16(sp+8u), el = MR16(sp+12u);
    if (!n4_core(c, th, a, b, MR32(sp+12u), &res)) return 0;
    if (el == 0xffffu){                                  /* the original writes to the argument slot */
        uint32_t P = a*0x64u + MR32(th+0x2cu), R = b*0x64u + MR32(P+4u);
        MR16(sp+12u) = MR16(R+0x54u);
    }
    c->eax = res; POP32(c->eip); c->esp += 12u;
    return 1;
}
/* Generic verification: the native code does a dry run (n4_dev RECORDS the
 * device calls instead of making them), memory and the call list are saved;
 * memory is restored, the original runs with dxrec recording; the memory
 * window (this +-256 KB, .data, extra records), the call sequence and
 * eax/esp are compared.  Only paths that reach the device solely through
 * n4_dev can be verified (guest sub-calls cannot be dry-run). */
static void nat_verify(CPU *c, int (*nat)(CPU*), void (*orig)(CPU*), uint32_t th,
                       const uint32_t *ext, int next, VStat *vs, const char *tag, uint32_t info){
    uint32_t lo = th & ~0x3fffu, hi = lo + 0x4000u, dn = V_DATA1 - V_DATA0;
    while (lo - 0x4000u >= th - V_WIN && mem_is_committed(lo - 0x4000u)) lo -= 0x4000u;
    while (hi + 0x4000u <= th + V_WIN && mem_is_committed(hi)) hi += 0x4000u;
    uint32_t n = hi - lo;
    static uint8_t *pre0, *pre1, *nat0, *nat1; static uint8_t preX[VEXT][0x64], natX[VEXT][0x64];
    if (!pre0){ pre0 = malloc(2u*V_WIN + 0x8000u); nat0 = malloc(2u*V_WIN + 0x8000u);
                pre1 = malloc(dn); nat1 = malloc(dn); }
    if (next > VEXT) next = VEXT;
    memcpy(pre0, g_mem + lo, n); memcpy(pre1, g_mem + V_DATA0, dn);
    for (int i = 0; i < next; i++) memcpy(preX[i], g_mem + ext[i], 0x64);
    CPU c0 = *c;
    n4_dry = 1; n4_dryn = 0;
    int ok = nat(c);
    n4_dry = 0;
    uint32_t neax = c->eax, nesp = c->esp;
    if (!ok){ *c = c0; orig(c); return; }
    memcpy(nat0, g_mem + lo, n); memcpy(nat1, g_mem + V_DATA0, dn);
    for (int i = 0; i < next; i++) memcpy(natX[i], g_mem + ext[i], 0x64);
    memcpy(g_mem + lo, pre0, n); memcpy(g_mem + V_DATA0, pre1, dn);
    for (int i = next - 1; i >= 0; i--) memcpy(g_mem + ext[i], preX[i], 0x64);
    *c = c0;
    DxRec vr; DxCall vc[NDRY]; uint8_t vd[256];
    DXREC_INIT(&vr, vc, vd);
    dxrec_begin(&vr, DXREC_STATE);
    orig(c);
    dxrec_end();
    if (!vr.ok){ vs->norec++; return; }     /* state block recording etc.: cannot compare */
    vs->runs++;
    /* this+0x114..0x11b (saved FPU CW + flag): 663a32 saves it inside
     * BeginPass and restores it at the end -- excluded, as in the Stage 1
     * verification. */
    { uint32_t off = th + 0x114u - lo; memcpy(nat0 + off, g_mem + th + 0x114u, 8); }
    int dm = memcmp(nat0, g_mem + lo, n) || memcmp(nat1, g_mem + V_DATA0, dn);
    for (int i = 0; !dm && i < next; i++) dm = memcmp(natX[i], g_mem + ext[i], 0x64) != 0;
    int dc = vr.ncall != n4_dryn;
    static const char *kn[13] = { "", "SetRenderState", "SetTextureStageState", "SetSamplerState", "SetTexture",
        "SetVertexShaderConstantF", "SetPixelShaderConstantF", "SetVertexShader", "SetPixelShader",
        "SetVertexShaderConstantI", "SetPixelShaderConstantI", "SetVertexShaderConstantB", "SetPixelShaderConstantB" };
    for (int i = 0; !dc && i < vr.ncall && i < NDRY; i++){
        const DxCall *k = &vr.call[i];
        int nk = (int)n4_dryk[i][0];
        int na = (nk >= 1 && nk <= 12) ? n4_nargs[nk] : 0;
        dc = na == 0 || strcmp(dxrec_mid_name(k->mid), kn[nk]) || k->a[0] != n4_dryk[i][1]
          || (na >= 2 && k->a[1] != n4_dryk[i][2]) || (na >= 3 && k->a[2] != n4_dryk[i][3]);
    }
    int de = c->eax != neax || c->esp != nesp;
    if (dm || dc || de){
        if (vs->bad++ < 12)
            fprintf(stderr, "[%s] diff (%08x): memory=%d calls=%d (native %d, original %d) eax=%d (%08x/%08x)\n",
                    tag, info, dm, dc, n4_dryn, vr.ncall, de, neax, c->eax);
        if (dm && vs->bad <= 4){ uint32_t z = ext && next ? ext[0] : 0; verify_diff(lo, n, nat0, th, z, z); }
    }
}
static VStat n4vs;
static uint64_t n4v_type[2], n4v_type_bad[2];      /* [1]: texture states (type 9) */
static void n4_verify(CPU *c){
    uint32_t th = c->ecx, sp = c->esp;
    uint32_t a = MR16(sp+4u), b = MR16(sp+8u);
    uint32_t P = a*0x64u + MR32(th+0x2cu), R = b*0x64u + MR32(P+4u);
    uint32_t ext[2] = { R, P }, ty = MR8(R+0x5bu), bad = (uint32_t)n4vs.bad;
    nat_verify(c, n4_apply, sub_00669286_orig, th, ext, 2, &n4vs, "D3DXN4VERIFY", MR32(R+0x58u));
    n4v_type[ty == 9u]++;
    if (n4vs.bad != bad) n4v_type_bad[ty == 9u]++;
}

void sub_00669286_orig(CPU *c);
void sub_00669286(CPU *c){
    if (g_n4_on < 0){
        const char *e = getenv("D3DXNATIVE4"); g_n4_on = e ? atoi(e) != 0 : 1;
        const char *v = getenv("D3DXN4VERIFY"); g_n4_v = v ? atoi(v) : 0;
    }
    if (g_n4_on && !g_pctx_n){
        uint64_t t0 = dx_on() ? mach_absolute_time() : 0;
        static uint64_t vk;
        if (g_n4_v && (++vk % (uint64_t)g_n4_v) == 0){
            uint32_t ty = MR32(MR16(c->esp+8u)*0x64u + MR32(MR16(c->esp+4u)*0x64u + MR32(c->ecx+0x2cu) + 4u) + 0x58u) >> 24;
            if (ty - 1u <= 2u || ty == 9u){ n4_verify(c); goto n4out; }
        }
        if (n4_apply(c)){
n4out:
            if (t0){ g_dx[DX_STATEAPPLY].ticks += mach_absolute_time() - t0; g_dx[DX_STATEAPPLY].n++; }
            return;
        }
    }
    if (g_m2_on < 0){
        const char *m = getenv("D3DXMEMO2"); g_m2_on = m ? atoi(m) != 0 : 1;
        const char *v = getenv("D3DXVERIFY2"); g_v2_n = v ? atoi(v) : 0;
        if (g_m2_on || g_v2_n) g_m2 = calloc(M2_CAP, sizeof *g_m2);
    }
    uint64_t t0 = dx_on() ? mach_absolute_time() : 0;
    if (!g_m2){
        if (g_pctx_n){ uint32_t th = c->ecx, a = MR16(c->esp+4u), b = MR16(c->esp+8u);
            uint32_t P = a*0x64u + MR32(th+0x2cu), R = b*0x64u + MR32(P+4u);
            pctx_note_state(th, P, R, (MR32(R+0x58u) >> 24) & 31u); }
        sub_00669286_orig(c); goto out; }
    {
    uint32_t th = c->ecx, a = MR16(c->esp+4u), b = MR16(c->esp+8u), el = MR16(c->esp+12u);
    uint32_t P = a*0x64u + MR32(th+0x2cu), R = b*0x64u + MR32(P+4u);
    uint32_t type = (MR32(R+0x58u) >> 24) & 31u;
    pctx_note_state(th, P, R, type);
    m2_calls++; m2_type_calls[type]++;
    /* Type 9 (indirect read from the texture table) and 14/15 (pick an object
     * from an array by expression and apply it nested) read inputs outside
     * the snapshot: D3DXVERIFY2 found device/memory differences for these
     * types.  Only the verified 1..8 are memoized; the rest always runs the
     * original code. */
    if (type == 0 || type > 8){ m2_excl++; sub_00669286_orig(c); goto out; }
    uint32_t ab = (a<<16) | b;
    uint32_t hk = (th * 0x9E3779B1u) ^ (ab * 0x85EBCA77u) ^ (el * 0xC2B2AE3Du);
    M2Slot *s = NULL;
    for (uint32_t j = 0; j < 64u; j++){
        M2Slot *t = &g_m2[(hk + j) & (M2_CAP-1)];
        if (!t->n || (t->th == th && t->ab == ab && t->elem == el)){ s = t; break; }
    }
    if (!s){ sub_00669286_orig(c); goto out; }
    if (!s->rec.call) DXREC_INIT(&s->rec, s->rcall, s->rdata);
    int mine = s->n && s->th == th && s->ab == ab && s->elem == el && s->valid;
    SnapW w = { s->buf, 0, M2_BUF, mine };
    /* claim the slot at once: a nested call must not take the same empty slot */
    s->th = th; s->ab = ab; s->elem = el; if (!s->n) s->n = 1;
    if (!state_apply_snap(&w, th, a, b, el, P, R)){
        m2_big++; s->valid = 0; sub_00669286_orig(c); goto out; }
    int same = mine && w.eq && w.len == s->len;
    int hit = 0;
    if (same){
        m2_same++;
        if (!s->rec.ok) m2_norec++;
        else if (!dxrec_redundant(&s->rec)) m2_devchg++;
        else hit = 1;
    }
    if (hit){
        m2_hit++; m2_type_hit[type]++;
        if (g_v2_n && (m2_hit % (uint64_t)g_v2_n) == 0){
            uint32_t lo = th & ~0x3fffu, hi = lo + 0x4000u, dn = V_DATA1 - V_DATA0;
            while (lo - 0x4000u >= th - V_WIN && mem_is_committed(lo - 0x4000u)) lo -= 0x4000u;
            while (hi + 0x4000u <= th + V_WIN && mem_is_committed(hi)) hi += 0x4000u;
            uint32_t n = hi - lo;
            if (!v_buf0){ v_buf0 = malloc(2u*V_WIN + 0x8000u); v_buf1 = malloc(dn); }
            memcpy(v_buf0, g_mem + lo, n); memcpy(v_buf1, g_mem + V_DATA0, dn);
            DxRec vr; DxCall vc[64]; uint8_t vd[1024];
            DXREC_INIT(&vr, vc, vd);
            dxrec_begin(&vr, DXREC_STATE);
            sub_00669286_orig(c);
            dxrec_end();
            v2_runs++;
            { uint32_t off = th + 0x114u - lo; memcpy(v_buf0 + off, g_mem + th + 0x114u, 8); }
            int dm = memcmp(v_buf0, g_mem + lo, n) || memcmp(v_buf1, g_mem + V_DATA0, dn);
            int dd = !vr.allred, de = c->eax != s->eax;
            if (dm || dd || de){
                v2_type_bad[type]++;
                if (dm) v2_mem++;
                if (dd) v2_dev++;
                if (de) v2_eax++;
                static int shown = 0;
                if (shown++ < 12)
                    fprintf(stderr, "[D3DXVERIFY2] type %u (code %08x) R=%08x: memory=%d device=%d eax=%d  calls %u (record ok=%d)\n",
                            type, MR32(R+0x58u), R, dm, dd, de, vr.ncall, vr.ok);
                if (dm && shown <= 4){
                    if (memcmp(v_buf0, g_mem + lo, n)) verify_diff(lo, n, v_buf0, th, P, R);
                }
            } else v2_clean++;
            s->eax = c->eax;
            goto out;
        }
        if (g_m2_on){
            m2_skip++;
            dxrec_append(&s->rec);        /* an open pass recording must know the skipped calls too */
            POP32(c->eip); c->esp += 12u; c->eax = s->eax;
            goto out;
        }
    }
    s->valid = 0;                         /* the buffer already holds the new snapshot */
    dxrec_begin(&s->rec, DXREC_STATE);
    sub_00669286_orig(c);
    dxrec_end();
    s->eax = c->eax; s->n++; s->type = type; s->len = w.len; s->valid = 1;
    }
out:
    if (t0){ g_dx[DX_STATEAPPLY].ticks += mach_absolute_time() - t0; g_dx[DX_STATEAPPLY].n++; }
}

/* ---- Stage 3: pass memo, sub_006691a1(this=ecx, pass, element) ret 8 ----
 * Hit: same fingerprint (effect fields + pass record), none of the dependent
 * parameter lines changed since the recording, all recorded device calls
 * are now no-ops.  Eligibility: every state in the pass is type 1..8, no
 * nested apply, 6691a1's special paths (P&8, [this+0x3c]==4 && P&1) are
 * off.
 *   D3DXMEMO3=1     : on (default OFF -- Stage 3 verification found wrong output)
 *   D3DXVERIFY3=<n> : on 1/n of the hits run the original, memory + device */
#define M3_BUF 192u
typedef struct {
    uint32_t th, ab, eax, n, len, valid, ep0;
    uint8_t  buf[M3_BUF];
    PassCtx  ctx;
    DxRec    rec;
    DxCall   rcall[64];
    uint8_t  rdata[1024];
} M3Slot;
#define M3_CAP 2048u
static M3Slot *g_m3;
static int g_m3_on = -1, g_v3_n = 0;
static uint64_t m3_calls, m3_fp, m3_dep, m3_dev, m3_inel, m3_hit, m3_skip, m3_rec, m3_recok;
static uint64_t v3_runs, v3_clean, v3_mem, v3_dev, v3_eax, m3_why[8]; static uint32_t m3_why_tm;

/* Fingerprint: effect fields, the pass record and the +0x8c..+0xab range of
 * the technique record.  With flag 8 set, 6691a1 calls 66403c/664197 on
 * every pass: they compare the pass's shader constant register counts
 * ([P+0x1c..]) with the technique's largest so far ([technique+0xa4..0xaa])
 * and do the record preparation only when it GROWS.  After warm-up there are
 * no calls; if it grows these fields change and the pass is recorded again. */
static inline int pass_fp(SnapW *w, uint32_t th, uint32_t P){
    uint32_t fix[5] = { th, MR32(th+0x2cu), MR32(th+0x3cu), MR32(th+0x40u), MR32(th+0x118u) & ~1u };
    uint32_t tech = MR16(th+0x42u)*0xb4u + MR32(th+0xa4u);
    return sw_put(w, fix, sizeof fix) && sw_put(w, g_mem + P, 0x64u)
        && sw_put(w, g_mem + tech + 0x8cu, 0x20u);
}

void sub_006691a1_orig(CPU *c);
/* ---- Stage 4b: native pass loop, sub_006691a1(this, a, el) ret 8 ----
 *   P = a*0x64 + [this+0x2c]
 *   [this+0x118]&8 : 66403c(P+0x1c, word[P+0x18]!=0xffff), 664197(P+0x24, word[P+0x1a]!=0xffff); <0 return
 *   P&8            : word[P+0x18]==0xffff ? 667d51(a) : 666df7(a); <0 return
 *   [this+0x3c]==4 : if P&1 return the result of 668dfd(P); otherwise
 *                    669286(a, b, el) for every state, return if <0
 *   other          : 669286 for every state; return if <0 and [this+0x3c]!=1
 *   0 at the end.  Type 1/2/3 states go directly through n4_core, the
 *   others through a guest call.  The original's prologue (ebp/ebx/esi/edi)
 *   is set up exactly.
 *   D3DXNATIVE5=0 turns it off.  With D3DXN4VERIFY on, the states go
 *   through the 669286 wrapper (and are verified there). */
void sub_0066403c(CPU*); void sub_00664197(CPU*); void sub_00667d51(CPU*);
void sub_00666df7(CPU*); void sub_00668dfd(CPU*);
static int g_n5_on = -1;
static uint64_t n5_calls;
static inline uint32_t n5_state(CPU *c, uint32_t th, uint32_t aw, uint32_t b, uint32_t el, uint32_t ret){
    uint32_t res;
    if (!g_n4_v && n4_core(c, th, aw & 0xffffu, b, el, &res)) return res;
    c->ecx = th; PUSH32(el); PUSH32(b); PUSH32(aw); PUSH32(ret);
    sub_00669286(c);
    return c->eax;
}
/* ---- Stage 9: a pass applied again with nothing changed ----
 * The game draws every mesh as Set*, CommitChanges, BeginPass, draw,
 * EndPass.  Among 32 friends' cars (FXTRACE of a frame) 5,196 of the 5,250
 * BeginPass calls re-applied the pass applied before and 127,541 of the
 * 128,532 state calls in passes were no-ops; the light streaks alone are
 * ~1,500 quads, mostly with no effect call between their passes but
 * CommitChanges.  A pass's application is a function of D3DX's memory and
 * the device state, and D3DX's memory changes only through calls to an
 * effect.  g_fx_epoch moves with every effect method the game calls
 * (wrappers in the dispatch table, below) but CommitChanges (it applies the
 * states a Set* queued, and that Set* moved it), EndPass (bookkeeping only),
 * BeginPass unless it switches the effect's pass (0x65AE25 walks the pass
 * records then), and SetTexture unless it changes a slot.  So when the epoch
 * has not moved since a pass's last application, applying it again would
 * evaluate every state to what it holds and make the device calls it made
 * then (recorded, no-ops included); when each of those is a no-op on the
 * device now, it changes nothing but D3DX's bookkeeping (dirty flags, the
 * saved control word) and is skipped.  Recorded only when every device call
 * went through n4_dev (texture states too, n4_core) and no pass was applied
 * inside it.  In a run among 32 friends' cars: 3.0 of 9.8 million passes.
 *   D3DXNATIVE9=0     : off
 *   D3DXN9VERIFY=<n>  : on 1/n of the skips the pass is applied with every
 *                       call recorded: all must be no-ops, and the effect
 *                       object and the pass's records unchanged. */
uint32_t g_fx_epoch = 1;
/* The lifted code calls effect methods through the dispatch table (cpu.h
 * DISPATCH_CALL): their entries there are replaced by a wrapper per vtable
 * slot (0x7B6FE0, 84 of them) that moves the epoch and calls what the table
 * held, but for BeginPass, CommitChanges and EndPass.  (With EFXSTAT those
 * entries are empty and dispatch_indirect, dispatch.c, moves it.) */
#define EFXW_N 84
static guest_fn_t g_efxw_next[EFXW_N];
#define EFXW(k) static void efxw_##k(CPU *c){ g_fx_epoch++; g_efxw_next[k](c); }
#define EFXW10(t) EFXW(t##0) EFXW(t##1) EFXW(t##2) EFXW(t##3) EFXW(t##4) EFXW(t##5) EFXW(t##6) EFXW(t##7) EFXW(t##8) EFXW(t##9)
EFXW(0) EFXW(1) EFXW(2) EFXW(3) EFXW(4) EFXW(5) EFXW(6) EFXW(7) EFXW(8) EFXW(9)
EFXW10(1) EFXW10(2) EFXW10(3) EFXW10(4) EFXW10(5) EFXW10(6) EFXW10(7)
EFXW(80) EFXW(81) EFXW(82) EFXW(83)
#define EFXR10(t) efxw_##t##0, efxw_##t##1, efxw_##t##2, efxw_##t##3, efxw_##t##4, efxw_##t##5, efxw_##t##6, efxw_##t##7, efxw_##t##8, efxw_##t##9,
static void (*const g_efxw[EFXW_N])(CPU *) = {
    efxw_0, efxw_1, efxw_2, efxw_3, efxw_4, efxw_5, efxw_6, efxw_7, efxw_8, efxw_9,
    EFXR10(1) EFXR10(2) EFXR10(3) EFXR10(4) EFXR10(5) EFXR10(6) EFXR10(7)
    efxw_80, efxw_81, efxw_82, efxw_83 };
#undef EFXW
#undef EFXW10
#undef EFXR10
void d3dx_hle_install(void){
    for (uint32_t k = 0; k < EFXW_N; k++){
        uint32_t a = MR32(0x007b6fe0u + 4u * k), o = a - DISPATCH_TEXT_BASE;
        if (a == 0x0066acd2u || a == 0x0066ac17u || a == 0x0065adcbu) continue;
        if (a == 0x0066d7d9u) continue;                   /* SetTexture moves it itself when it changes a slot */
        if (o >= DISPATCH_TEXT_SIZE || !g_dtab[o]) continue;
        int mine = 0;
        for (uint32_t q = 0; q < EFXW_N; q++) if (g_dtab[o] == g_efxw[q]) mine = 1;
        if (mine) continue;                               /* a method in two slots */
        g_efxw_next[k] = g_dtab[o]; g_dtab[o] = g_efxw[k];
    }
}
#define PR_CAP 128u
static PassRep g_pr[PR_CAP];
static int g_n9_on = -1, g_n9_v = 0;
static uint64_t n9_pass, n9_skip, n9_rec, n9_bad;
static VStat n9vs;
extern uint64_t g_com_calls;
static inline int pr_redundant(const PassRep *p){
    for (uint32_t i = 0; i < p->n; i++){
        const PrCall *k = &p->c[i];
        int f = n4_red_fast(k->kind, k->a0, k->a1, k->a2);
        if (!f || (f < 0 && !dxn_skip(k->kind, k->a0, k->a1, k->a2))) return 0;
    }
    return 1;
}
static void n5_pass(CPU *c){
    uint32_t th = c->ecx, sp = c->esp, ret = MR32(sp), aw = MR32(sp+4u), el = MR32(sp+8u);
    uint32_t a = aw & 0xffffu, P = a*0x64u + MR32(th+0x2cu), r = 0;
    uint32_t sebp = c->ebp, sebx = c->ebx, sesi = c->esi, sedi = c->edi;
    PUSH32(sebp); uint32_t fp = c->esp; PUSH32(sebx); PUSH32(sesi); PUSH32(sedi);
    c->ebp = fp; c->ebx = a; c->esi = P; c->edi = th;
    n5_calls++;
    if (MR8(th+0x118u) & 8u){
        c->ecx = th; PUSH32(P+0x1cu); PUSH32(MR16(P+0x18u) != 0xffffu); PUSH32(0x6691d5u);
        sub_0066403c(c); r = c->eax; if ((int32_t)r < 0) goto done;
        c->ecx = th; PUSH32(P+0x24u); PUSH32(MR16(P+0x1au) != 0xffffu); PUSH32(0x6691f4u);
        sub_00664197(c); r = c->eax; if ((int32_t)r < 0) goto done;
    }
    if (MR8(P+8u) & 8u){
        c->ecx = th; PUSH32(a);
        if (MR16(P+0x18u) == 0xffffu){ PUSH32(0x669231u); sub_00667d51(c); }
        else { PUSH32(0x669212u); sub_00666df7(c); }
        r = c->eax; if ((int32_t)r < 0) goto done;
    }
    if (g_n9_on < 0){
        const char *e = getenv("D3DXNATIVE9"); g_n9_on = e ? atoi(e) != 0 : 1;
        const char *v = getenv("D3DXN9VERIFY"); g_n9_v = v ? atoi(v) : 0;
    }
    PassRep *pr = NULL;
    DxRec vr; DxCall vc[64]; uint8_t vd[2048]; int vfy = 0;
    static uint8_t w0[0x200u + 0x64u * 64u]; uint32_t wP = 0, wn = 0;
    uint64_t cc = 0, ii = 0;
    if (g_pr_cur) g_pr_cur->bad = 1;                    /* a pass inside a recorded one */
    else if (g_n9_on && !(MR8(P+8u) & 8u) && !n4_dry && !g_n4_v && !(g_fxtr && fxtr_now())){
        uint32_t mode = MR32(th+0x3cu);
        pr = &g_pr[((th * 0x9E3779B1u) ^ (a * 0x85EBCA77u) ^ el) & (PR_CAP-1u)];
        n9_pass++;
        if (pr->ok && pr->th == th && pr->a == a && pr->el == el && pr->mode == mode
            && pr->epoch == g_fx_epoch && pr_redundant(pr)){
            static uint64_t vk;
            if (!g_n9_v || (++vk % (uint64_t)g_n9_v) != 0){ n9_skip++; r = 0; goto done; }
            vfy = 1;                                     /* apply it, recorded, and compare */
            wP = MR32(P+4u); wn = MR16(P+0x36u); if (wn > 64u) wn = 64u;
            memcpy(w0, g_mem + th, 0x200u);
            if (wn && mem_is_committed(wP + wn*0x64u - 1u)) memcpy(w0 + 0x200u, g_mem + wP, wn*0x64u); else wn = 0;
            DXREC_INIT(&vr, vc, vd);
            dxrec_begin(&vr, DXREC_PASS);
        }
        pr->th = th; pr->a = a; pr->el = el; pr->mode = mode; pr->n = 0; pr->ok = 0; pr->bad = 0;
        cc = g_com_calls; ii = n4_issued;
        g_pr_cur = pr;
    }
    if (MR32(th+0x3cu) == 4u){
        if (MR8(P+8u) & 1u){
            c->ecx = th; PUSH32(P); PUSH32(0x66922au); sub_00668dfd(c);
            r = c->eax; goto done;
        }
        for (uint32_t b = 0; b < MR16(P+0x36u); b++){
            r = n5_state(c, th, aw, b, el, 0x669249u);
            if ((int32_t)r < 0) goto done;
        }
    } else {
        for (uint32_t b = 0; b < MR16(P+0x36u); b++){
            r = n5_state(c, th, aw, b, el, 0x66926cu);
            if ((int32_t)r < 0 && MR32(th+0x3cu) != 1u) goto done;
        }
    }
    r = 0;
done:
    if (pr && g_pr_cur == pr){
        g_pr_cur = NULL;
        pr->ok = r == 0 && !pr->bad && g_com_calls - cc == n4_issued - ii;
        pr->epoch = g_fx_epoch;
        n9_rec++; if (!pr->ok) n9_bad++;
        if (vfy){
            dxrec_end();
            if (!vr.ok) n9vs.norec++;
            else {
                n9vs.runs++;
                memcpy(w0 + 0x114u, g_mem + th + 0x114u, 8);    /* the control word saved and restored by BeginPass */
                int dm = memcmp(w0, g_mem + th, 0x200u) || (wn && memcmp(w0 + 0x200u, g_mem + wP, wn*0x64u));
                if (dm || !vr.allred || r){
                    if (n9vs.bad++ < 12){
                        fprintf(stderr, "[D3DXN9VERIFY] effect %08x pass %u: memory=%d, %u calls, not all no-ops=%d, result %08x\n",
                                th, a, dm, vr.ncall, !vr.allred, r);
                        for (int q = 0; q < vr.ncall; q++) if (!vr.call[q].red)
                            fprintf(stderr, "      call made: %s(%x, %x, %x)\n", dxrec_mid_name(vr.call[q].mid),
                                    vr.call[q].a[0], vr.call[q].a[1], vr.call[q].a[2]);
                    }
                    if (dm && n9vs.bad <= 4){
                        verify_diff(th, 0x200u, w0, th, P, P);
                        if (wn) verify_diff(wP, wn*0x64u, w0 + 0x200u, th, P, P);
                    }
                }
            }
        }
    }
    c->eax = r;
    c->edi = sedi; c->esi = sesi; c->ebx = sebx; c->ebp = sebp;
    c->esp = sp + 12u; c->eip = ret;
}

void sub_006691a1(CPU *c){
    if (g_n5_on < 0){ const char *e = getenv("D3DXNATIVE5"); g_n5_on = e ? atoi(e) != 0 : 1; }
    if (g_n5_on && g_n4_on > 0 && !g_m3_on && !g_pctx_n){
        uint64_t t0 = dx_on() ? mach_absolute_time() : 0;
        n5_pass(c);
        if (t0){ g_dx[DX_PASSAPPLY].ticks += mach_absolute_time() - t0; g_dx[DX_PASSAPPLY].n++; }
        return;
    }
    if (g_m3_on < 0){
        /* DEFAULT OFF: D3DXVERIFY3 showed that skipping at the pass level is
         * wrong (textures come from the object table, not from parameter
         * data; 78% of the predicted hits were really different).
         * D3DXMEMO3=1 for research only. */
        const char *m = getenv("D3DXMEMO3"); g_m3_on = m ? atoi(m) != 0 : 0;
        const char *v = getenv("D3DXVERIFY3"); g_v3_n = v ? atoi(v) : 0;
        if (g_m3_on || g_v3_n){ g_m3 = calloc(M3_CAP, sizeof *g_m3); g_pv_on = 1; }
    }
    uint64_t t0 = dx_on() ? mach_absolute_time() : 0;
    if (!g_m3){ sub_006691a1_orig(c); goto out3; }
    {
    uint32_t th = c->ecx, a = MR16(c->esp+4u), el = MR16(c->esp+8u);
    uint32_t P = a*0x64u + MR32(th+0x2cu);
    m3_calls++;
    uint32_t ab = (a<<16) | el;
    uint32_t hk = (th * 0x9E3779B1u) ^ (ab * 0x85EBCA77u);
    M3Slot *s = NULL;
    for (uint32_t j = 0; j < 32u; j++){
        M3Slot *t = &g_m3[(hk + j) & (M3_CAP-1)];
        if (!t->n || (t->th == th && t->ab == ab)){ s = t; break; }
    }
    if (!s){ sub_006691a1_orig(c); goto out3; }
    if (!s->rec.call) DXREC_INIT(&s->rec, s->rcall, s->rdata);
    int mine = s->n && s->th == th && s->ab == ab;
    SnapW w = { s->buf, 0, M3_BUF, mine && s->len };
    s->th = th; s->ab = ab; if (!s->n) s->n = 1;
    if (!pass_fp(&w, th, P)){ s->valid = 0; sub_006691a1_orig(c); goto out3; }
    int hit = 0;
    if (mine && s->valid){
        if (!(w.eq && w.len == s->len)) m3_fp++;
        else {
            uint32_t k = 0;
            for (; k < s->ctx.ndep; k++) if (pv_get(s->ctx.dep[k]) > s->ep0) break;
            if (k < s->ctx.ndep) m3_dep++;
            else if (!dxrec_redundant(&s->rec)) m3_dev++;
            else hit = 1;
        }
    } else if (mine) m3_inel++;
    if (hit){
        m3_hit++;
        if (g_v3_n && (m3_hit % (uint64_t)g_v3_n) == 0){
            uint32_t lo = th & ~0x3fffu, hi = lo + 0x4000u, dn = V_DATA1 - V_DATA0;
            while (lo - 0x4000u >= th - V_WIN && mem_is_committed(lo - 0x4000u)) lo -= 0x4000u;
            while (hi + 0x4000u <= th + V_WIN && mem_is_committed(hi)) hi += 0x4000u;
            uint32_t n = hi - lo;
            if (!v_buf0){ v_buf0 = malloc(2u*V_WIN + 0x8000u); v_buf1 = malloc(dn); }
            memcpy(v_buf0, g_mem + lo, n); memcpy(v_buf1, g_mem + V_DATA0, dn);
            static DxRec vr; static DxCall vc[256]; static uint8_t vd[4096];
            DXREC_INIT(&vr, vc, vd);
            dxrec_begin(&vr, DXREC_PASS);
            sub_006691a1_orig(c);
            dxrec_end();
            v3_runs++;
            { uint32_t off = th + 0x114u - lo; memcpy(v_buf0 + off, g_mem + th + 0x114u, 8); }
            int dm = memcmp(v_buf0, g_mem + lo, n) || memcmp(v_buf1, g_mem + V_DATA0, dn);
            int dd = !vr.allred, de = c->eax != s->eax;
            if (dm || dd || de){
                if (dm) v3_mem++;
                if (dd) v3_dev++;
                if (de) v3_eax++;
                static int shown = 0;
                if (shown++ < 12){
                    fprintf(stderr, "[D3DXVERIFY3] this=%08x pass=%u: memory=%d device=%d eax=%d calls %u (record %u) dependencies %u\n",
                            th, a, dm, dd, de, vr.ncall, s->rec.ncall, s->ctx.ndep);
                    for (int q = 0; q < vr.ncall; q++) if (!vr.call[q].red){
                        const DxCall *k = &vr.call[q];
                        uint32_t old = 0xFFFFFFFFu;
                        for (int z = 0; z < s->rec.ncall; z++)
                            if (s->rec.call[z].mid == k->mid && s->rec.call[z].a[0] == k->a[0]
                                && (k->mid != s->rec.call[z].mid || s->rec.call[z].a[1] == k->a[1] || 1)){ old = s->rec.call[z].a[2]; if (s->rec.call[z].a[1] != k->a[1]) old = s->rec.call[z].a[1]; break; }
                        fprintf(stderr, "      effective call: %s(%u, %u, %u)  recorded: %08x\n",
                                dxrec_mid_name(k->mid), k->a[0], k->a[1], k->a[2], old);
                    }
                    if (dm && shown <= 3 && memcmp(v_buf0, g_mem + lo, n)) verify_diff(lo, n, v_buf0, th, P, P);
                }
            } else v3_clean++;
            goto out3;
        }
        if (g_m3_on){
            m3_skip++;
            dxrec_append(&s->rec);
            POP32(c->eip); c->esp += 8u; c->eax = s->eax;
            goto out3;
        }
    }
    /* record: collect the dependencies and the device calls */
    m3_rec++;
    s->valid = 0; s->len = w.len;
    s->ep0 = g_pepoch;
    s->ctx.elig = !(MR32(P+8u) & 8u) && !(MR32(th+0x3cu) == 4u && (MR32(P+8u) & 1u));
    s->ctx.ndep = 0; s->ctx.tmask = 0;
    if (g_pctx_n < 8) g_pctx[g_pctx_n] = &s->ctx; else s->ctx.elig = 0;
    g_pctx_n++;
    dxrec_begin(&s->rec, DXREC_PASS);
    sub_006691a1_orig(c);
    dxrec_end();
    g_pctx_n--;
    s->eax = c->eax;
    /* if a parameter changed during the recording (it should not), do not trust it */
    s->valid = s->rec.ok && s->ctx.elig && s->ep0 == g_pepoch;
    if (s->valid) m3_recok++;
    else {
        if (!s->rec.ok) m3_why[0]++;
        if (!s->ctx.elig) m3_why[1]++;
        if (s->ep0 != g_pepoch) m3_why[2]++;
        if (MR32(th+0x118u) & 8u) m3_why[3]++;
        if (MR32(P+8u) & 8u) m3_why[4]++;
        if (MR32(th+0x3cu) == 4u && (MR32(P+8u) & 1u)) m3_why[5]++;
        if (s->rec.ncall >= s->rec.maxcall) m3_why[6]++;
        m3_why_tm |= s->ctx.tmask;
    }
    }
out3:
    if (t0){ g_dx[DX_PASSAPPLY].ticks += mach_absolute_time() - t0; g_dx[DX_PASSAPPLY].n++; }
}

/* ---- Guest thread CPU time: a measurement independent of GPU waits ----
 * CPUWIN=<first>[,<last>]: the thread CPU clock is read at Present (guest
 * thread); the report prints the CPU ms per frame of frames [first, last).
 * The frame time is max(CPU, GPU), so when the GPU is shared with another
 * application this is the only reliable CPU measure.                     */
#include <time.h>
static long cw_from = -2, cw_f0 = -1, cw_f1 = -1; static long long cw_to = 0;
static double cw_t0, cw_t1;
/* per-thread instruction/cycle counter (libsystem_kernel thread_selfcounts):
 * far more stable than time; the efficiency measure of the translator's code */
#include <dlfcn.h>
static uint64_t cw_i0[2], cw_i1[2];
static int (*cw_tsc)(int, void *, size_t);
static void cw_counts(uint64_t *v){
    static int init = 0;
    if (!init){ init = 1; cw_tsc = (int (*)(int, void *, size_t))dlsym(RTLD_DEFAULT, "thread_selfcounts"); }
    v[0] = v[1] = 0;
    if (cw_tsc) cw_tsc(1, v, 2 * sizeof(uint64_t));
}
void guest_cpu_mark(void){
    if (cw_from == -2){
        const char *e = getenv("CPUWIN"); cw_from = -1;
        if (e){ cw_from = atol(e); const char *k = strchr(e, ','); cw_to = k ? atoll(k+1) : 1LL<<40; }
    }
    if (cw_from < 0) return;
    long f = metal_frames();
    if (f < cw_from || (cw_f1 >= 0 && f > cw_to)) return;
    struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    double t = ts.tv_sec*1e3 + ts.tv_nsec/1e6;
    if (cw_f0 < 0){ cw_f0 = f; cw_t0 = t; cw_counts(cw_i0); }
    if (f <= cw_to){ cw_f1 = f; cw_t1 = t; cw_counts(cw_i1); }
}

/* ---- EFXSTAT=<frame>: inventory of the ID3DXEffect vtable (0x7b6fe8, 82 methods) ----
 * Every effect method call that comes through dispatch_indirect: calls/frame
 * per method (from the game / from inside D3DX), distinct effect objects,
 * Begin flags, distinct (effect, pass) pairs.  Diagnostics only.        */
#define EFX_NM 82
int g_efx_stat;
static long long efx_from; static long efx_f0 = -1;
static uint32_t efx_vt[EFX_NM];
static uint64_t efx_n[EFX_NM][2], efx_hstr[EFX_NM];
static uint32_t efx_this[1024]; static int efx_nthis;
static uint64_t efx_pair[4096]; static int efx_npair;
static uint32_t efx_bflag[8], efx_bflag_n[8]; static int efx_nbf;
extern int g_fxtr; int fxtr_now(void);
__attribute__((constructor)) static void efx_init(void){
    const char *e = getenv("EFXSTAT");
    g_efx_stat = e != NULL || getenv("FXTRACE") != NULL;
    efx_from = e ? atoll(e) : (1LL << 40);
}
/* FXTRACE: effect method marker (in the same stream as com.c's device call trace) */
static void efx_trace(CPU *c, int k){
    static const char *nm[EFX_NM] = { [24]="SetInt", [26]="SetIntArray", [28]="SetFloat",
        [30]="SetFloatArray", [32]="SetVector", [34]="SetVectorArray", [36]="SetMatrix",
        [38]="SetMatrixArray", [50]="SetTexture", [58]="SetTechnique", [63]="Begin",
        [64]="BeginPass", [65]="CommitChanges", [66]="EndPass", [67]="End" };
    uint32_t sp = c->esp, th = MR32(sp+4u), a1 = MR32(sp+8u), a2 = MR32(sp+12u), a3 = MR32(sp+16u);
    fprintf(stderr, "E %s[%d] %08x %08x", nm[k] ? nm[k] : "?", k, th, a1);
    uint32_t nf = 0, p = a2;
    switch (k){
    case 24: case 50: case 58: case 63: fprintf(stderr, " %x", a2); break;
    case 28: { float f; memcpy(&f, &a2, 4); fprintf(stderr, " %g", f); } break;
    case 26: fprintf(stderr, " n=%u:", a3);
        for (uint32_t i = 0; i < a3 && i < 64 && mem_is_committed(a2+4u*i+3u); i++) fprintf(stderr, " %d", (int)MR32(a2+4u*i)); break;
    case 30: nf = a3; fprintf(stderr, " n=%u", a3); break;
    case 32: nf = 4; break;
    case 34: nf = 4u*a3; fprintf(stderr, " n=%u", a3); break;
    case 36: nf = 16; break;
    case 38: nf = 16u*a3; fprintf(stderr, " n=%u", a3); break;
    }
    for (uint32_t i = 0; i < nf && i < 256 && mem_is_committed(p+4u*i) && mem_is_committed(p+4u*i+3u); i++){
        float f; uint32_t u = MR32(p+4u*i); memcpy(&f, &u, 4);
        fprintf(stderr, "%s%g", (i&3) ? "," : " ", f);
    }
    fprintf(stderr, "\n");
}
static void efx_set_add(uint32_t *s, int *n, int cap, uint32_t v){
    for (int i = 0; i < *n; i++) if (s[i] == v) return;
    if (*n < cap) s[(*n)++] = v;
}
void efx_stat_note(CPU *c, uint32_t addr){
    if (!efx_vt[0]) for (int i = 0; i < EFX_NM; i++) efx_vt[i] = MR32(0x7b6fe8u + 4u*i);
    int k = -1;
    for (int i = 0; i < EFX_NM; i++) if (efx_vt[i] == addr){ k = i; break; }
    if (k < 0) return;
    if (g_fxtr && fxtr_now()) efx_trace(c, k);
    if (metal_frames() < efx_from) return;
    if (efx_f0 < 0) efx_f0 = metal_frames();
    uint32_t ret = MR32(c->esp), th = MR32(c->esp+4u);
    efx_n[k][ret - 0x650000u < 0x40000u]++;
    efx_set_add(efx_this, &efx_nthis, 1024, th);
    /* is the handle a string?  (in slots 8..64 the first argument is a D3DXHANDLE) */
    if (k >= 8 && k <= 64){
        uint32_t h = MR32(c->esp+8u);
        int str = h >= 0x10000u && mem_is_committed(h) && mem_is_committed(h+7u);
        for (int i = 0; str && i < 8; i++){
            uint8_t ch = MR8(h+i); if (!ch){ str = i > 0; break; }
            if (!((ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||ch=='_')) str = 0;
        }
        efx_hstr[k] += str;
    }
    if (k == 63){                                    /* Begin(this, pPasses, flags) */
        uint32_t f = MR32(c->esp+12u); int j = 0;
        for (; j < efx_nbf; j++) if (efx_bflag[j] == f) break;
        if (j == efx_nbf && efx_nbf < 8) efx_bflag[efx_nbf++] = f;
        if (j < 8) efx_bflag_n[j]++;
    }
    if (k == 64){                                    /* BeginPass(this, pass) */
        uint64_t p = ((uint64_t)th << 32) | MR32(c->esp+8u);
        int j = 0; for (; j < efx_npair; j++) if (efx_pair[j] == p) break;
        if (j == efx_npair && efx_npair < 4096) efx_pair[efx_npair++] = p;
    }
}
/* dispatch.c: an indirect call that did not come through the dispatch table
 * (dispatch_indirect), into UG2's statically linked D3DX code 0x650000-
 * 0x690000: EFXSTAT's inventory, and the effect epoch for every method but
 * BeginPass, CommitChanges and EndPass (see g_fx_epoch) */
#define FX_CODE(a) ((a) - 0x650000u < 0x40000u)
int efx_watched(uint32_t a){ return g_efx_stat && FX_CODE(a); }
void efx_dispatch(CPU *c, uint32_t addr){
    if (!FX_CODE(addr)) return;
    if (g_efx_stat) efx_stat_note(c, addr);
    if (addr != 0x0066acd2u && addr != 0x0066ac17u && addr != 0x0065adcbu) g_fx_epoch++;
}
/* D3DXCreateEffectFromResourceA(device, module, resource, defines, include,
 * flags, pool, &effect, &errors)  stdcall, 9 arguments. */
void sub_00640c02_orig(CPU *c);
void sub_00640c02(CPU *c){
    uint32_t sp = c->esp, res = MR32(sp+12u), pool = MR32(sp+28u), pout = MR32(sp+32u);
    sub_00640c02_orig(c);
    if (g_efx_stat){
        char nm[64] = "#";
        if (res >= 0x10000u){ int i = 0; for (; i < 63 && MR8(res+i); i++) nm[i] = (char)MR8(res+i); nm[i] = 0; }
        else snprintf(nm, sizeof nm, "#%u", res);
        uint32_t ef = pout ? MR32(pout) : 0u;
        fprintf(stderr, "[EFXSTAT] create %-28s pool=%08x effect=%08x result=%08x frame=%ld"
                " param %u@%08x technique %u@%08x object@%08x\n",
                nm, pool, ef, c->eax, metal_frames(), ef ? MR32(ef+0x98u) : 0u, ef ? MR32(ef+0x9cu) : 0u,
                ef ? MR32(ef+0xa0u) : 0u, ef ? MR32(ef+0xa4u) : 0u, ef ? MR32(ef+0x18u) : 0u);
    }
}

static void efx_report(void){
    if (!g_efx_stat || efx_f0 < 0) return;
    long fr = metal_frames() - efx_f0; if (fr < 1) fr = 1;
    fprintf(stderr, "\n=== EFXSTAT: ID3DXEffect calls, %ld frames (from frame %ld) ===\n", fr, efx_f0);
    fprintf(stderr, "  slot  address    game/frame  in-D3DX/frame   total   text-handle\n");
    for (int i = 0; i < EFX_NM; i++) if (efx_n[i][0] || efx_n[i][1])
        fprintf(stderr, "  %3d   %08x  %10.1f  %10.1f  %8llu  %8llu\n", i, efx_vt[i],
                (double)efx_n[i][0]/fr, (double)efx_n[i][1]/fr,
                (unsigned long long)(efx_n[i][0] + efx_n[i][1]), (unsigned long long)efx_hstr[i]);
    fprintf(stderr, "  distinct effect objects: %d | distinct (effect, pass): %d\n", efx_nthis, efx_npair);
    fprintf(stderr, "  Begin flags:");
    for (int j = 0; j < efx_nbf; j++) fprintf(stderr, " 0x%x x%u", efx_bflag[j], efx_bflag_n[j]);
    fprintf(stderr, "\n");
}

void d3dx_hle_report(void){
    efx_report();
    { extern void comstat_report(void); comstat_report(); }
    if (cw_f1 > cw_f0 && cw_f0 >= 0)
        fprintf(stderr, "\n=== CPUWIN: guest thread CPU %.3f ms/frame (%ld..%ld, %ld frames) ===\n",
                (cw_t1 - cw_t0) / (double)(cw_f1 - cw_f0), cw_f0, cw_f1, cw_f1 - cw_f0);
        if (cw_i1[0] > cw_i0[0]){
            double fr = (double)(cw_f1 - cw_f0), ki = (double)(cw_i1[0] - cw_i0[0]), kc = (double)(cw_i1[1] - cw_i0[1]);
            fprintf(stderr, "=== CPUWIN: guest thread %.3f M instructions/frame, %.3f M cycles/frame, IPC %.2f ===\n",
                    ki / fr / 1e6, kc / fr / 1e6, ki / kc);
        }
    if (m3_calls){
        fprintf(stderr, "  --- pass memo (sub_006691a1, stage 3) ---\n");
        fprintf(stderr, "  calls %llu | hits %llu (%.1f%%) | skipped %llu | recorded %llu (eligible %llu)\n"
                        "  misses: fingerprint %llu, parameter changed %llu, device changed %llu, not eligible %llu\n"
                        "  version: %llu parameter changes, table full %llu\n",
            (unsigned long long)m3_calls, (unsigned long long)m3_hit, 100.0*m3_hit/m3_calls,
            (unsigned long long)m3_skip, (unsigned long long)m3_rec, (unsigned long long)m3_recok,
            (unsigned long long)m3_fp, (unsigned long long)m3_dep, (unsigned long long)m3_dev,
            (unsigned long long)m3_inel, (unsigned long long)pv_bumps, (unsigned long long)pv_full);
        fprintf(stderr, "  ineligible records: rec %llu, type/dependency %llu, parameter %llu, flag8 %llu, P&8 %llu, 3c==4 %llu, call overflow %llu, types seen %08x\n",
            (unsigned long long)m3_why[0], (unsigned long long)m3_why[1], (unsigned long long)m3_why[2],
            (unsigned long long)m3_why[3], (unsigned long long)m3_why[4], (unsigned long long)m3_why[5],
            (unsigned long long)m3_why[6], m3_why_tm);
        if (v3_runs) fprintf(stderr, "  VERIFY3: %llu runs, clean %llu | memory %llu, device %llu, eax %llu\n",
            (unsigned long long)v3_runs, (unsigned long long)v3_clean, (unsigned long long)v3_mem,
            (unsigned long long)v3_dev, (unsigned long long)v3_eax);
    }
    if (n4_calls || n4vs.runs){
        fprintf(stderr, "  --- native state applier (669286 types 1/2/3, stage 4a) ---\n"
                        "  calls %llu | dirty (664a1f) %llu | device calls made %llu, skipped (no effect) %llu\n",
            (unsigned long long)n4_calls, (unsigned long long)n4_dirty,
            (unsigned long long)n4_issued, (unsigned long long)n4_skipped);
        fprintf(stderr, "  native pass loop (6691a1, stage 4b): %llu | texture states (type 9): %llu\n", (unsigned long long)n5_calls, (unsigned long long)n4_tex);
        if (n9_pass) fprintf(stderr, "  repeated passes (stage 9): %llu passes, %llu skipped, %llu applied and recorded (%llu not replayable)\n",
            (unsigned long long)n9_pass, (unsigned long long)n9_skip, (unsigned long long)n9_rec, (unsigned long long)n9_bad);
        if (n9vs.runs || n9vs.norec) fprintf(stderr, "  VERIFY-9: %llu skips applied in full, DIFFERENT %llu (not compared %llu)\n",
            (unsigned long long)n9vs.runs, (unsigned long long)n9vs.bad, (unsigned long long)n9vs.norec);
        fprintf(stderr, "  expression memo: hits %llu, misses %llu, not memoizable %llu (not an expression %llu, no inputs %llu, inputs>2 %llu, >96 bytes %llu)\n",
            (unsigned long long)n4m_hit, (unsigned long long)n4m_miss, (unsigned long long)n4m_big,
            (unsigned long long)n4m_why[0], (unsigned long long)n4m_why[1], (unsigned long long)n4m_why[2], (unsigned long long)n4m_why[3]);
        if (n4vs.runs) fprintf(stderr, "  VERIFY-4: %llu runs, DIFFERENT %llu (record invalid, not compared: %llu); texture states %llu, DIFFERENT %llu\n",
            (unsigned long long)n4vs.runs, (unsigned long long)n4vs.bad, (unsigned long long)n4vs.norec,
            (unsigned long long)n4v_type[1], (unsigned long long)n4v_type_bad[1]);
        if (n8_set || n8_tex) fprintf(stderr, "  native SetTexture: %llu | texture slot (65ccc8): %llu | skipped texture AddRef/Release: %llu\n",
            (unsigned long long)n8_set, (unsigned long long)n8_tex, (unsigned long long)n8_refskip);
        if (n7vs.runs || n7_calls) fprintf(stderr, "  native shader applier (668dfd): %llu | VERIFY-7: %llu runs, DIFFERENT %llu (not compared %llu) | object state (types 14/15): %llu\n",
            (unsigned long long)n7_calls, (unsigned long long)n7vs.runs, (unsigned long long)n7vs.bad, (unsigned long long)n7vs.norec, (unsigned long long)n4_obj);
        if (n6vs.runs || n6_calls) fprintf(stderr, "  native sampler applier (668cc0): %llu | VERIFY-6: %llu runs, DIFFERENT %llu (not compared %llu)\n",
            (unsigned long long)n6_calls, (unsigned long long)n6vs.runs, (unsigned long long)n6vs.bad, (unsigned long long)n6vs.norec);
    }
    if (mm_calls || pl_calls || sm_calls){
        fprintf(stderr, "  --- native helpers ---\n  matrix multiply %llu (slow path %llu) | pool declaration %llu"
                        " | SetMatrix conversion %llu (slow path %llu)\n",
            (unsigned long long)mm_calls, (unsigned long long)mm_slow, (unsigned long long)pl_calls,
            (unsigned long long)sm_calls, (unsigned long long)sm_slow);
        if (mmv_runs || plv_runs || smv_runs)
            fprintf(stderr, "  VERIFY-N: matrix %llu runs, DIFFERENT %llu | pool %llu runs, DIFFERENT %llu"
                            " | conversion %llu runs, DIFFERENT %llu\n",
                (unsigned long long)mmv_runs, (unsigned long long)mmv_bad,
                (unsigned long long)plv_runs, (unsigned long long)plv_bad,
                (unsigned long long)smv_runs, (unsigned long long)smv_bad);
    }
    if (m2_calls){
        fprintf(stderr, "  --- state apply memo (sub_00669286) ---\n");
        fprintf(stderr, "  calls %llu | same inputs %llu (%.1f%%) | hits %llu (%.1f%%) | skipped %llu\n"
                        "  record invalid %llu | device changed %llu | buffer overflow %llu | type excluded %llu\n",
            (unsigned long long)m2_calls, (unsigned long long)m2_same, 100.0*m2_same/m2_calls,
            (unsigned long long)m2_hit, 100.0*m2_hit/m2_calls, (unsigned long long)m2_skip,
            (unsigned long long)m2_norec, (unsigned long long)m2_devchg, (unsigned long long)m2_big,
            (unsigned long long)m2_excl);
        fprintf(stderr, "  type: calls/hit%%");
        for (int i = 0; i < 32; i++) if (m2_type_calls[i])
            fprintf(stderr, "  %d:%llu/%.0f%%", i, (unsigned long long)m2_type_calls[i],
                    100.0*m2_type_hit[i]/m2_type_calls[i]);
        fprintf(stderr, "\n");
        if (v2_runs){
            fprintf(stderr, "  VERIFY2: %llu runs, clean %llu | memory %llu, device %llu, eax %llu\n",
                (unsigned long long)v2_runs, (unsigned long long)v2_clean, (unsigned long long)v2_mem,
                (unsigned long long)v2_dev, (unsigned long long)v2_eax);
            fprintf(stderr, "  failing types:");
            for (int i = 0; i < 32; i++) if (v2_type_bad[i]) fprintf(stderr, " %d:%llu", i, (unsigned long long)v2_type_bad[i]);
            fprintf(stderr, "\n");
        }
    }
    if (m_calls){
        fprintf(stderr, "  --- state value memo (sub_00664a1f) ---\n");
        if (m_big) fprintf(stderr, "  inputs over %u bytes (not memoized): %llu\n", MEMO_BUF, (unsigned long long)m_big);
        fprintf(stderr, "  calls %llu | eligible %llu | hits %llu (%.1f%%) | skipped %llu | guard cancels %llu\n",
            (unsigned long long)m_calls, (unsigned long long)m_elig, (unsigned long long)m_hit,
            m_elig ? 100.0*m_hit/m_elig : 0.0, (unsigned long long)m_skip, (unsigned long long)m_guard);
        if (v_runs) fprintf(stderr, "  VERIFY: %llu runs, clean %llu, DIFFERENT %llu\n",
            (unsigned long long)v_runs, (unsigned long long)v_clean, (unsigned long long)v_dirty);
    }
    if (!dx_on()) return;
    long fr = metal_frames() - (g_dx_f0 < 0 ? 0 : g_dx_f0); if (fr < 1) fr = 1;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    fprintf(stderr, "\n=== D3DX effect (D3DXSTAT) — %ld frames, from frame %ld ===\n", fr, g_dx_f0);
    fprintf(stderr, "  %-20s %10s %12s %10s %10s\n",
            "function", "calls/frame", "wrapper", "ms/frame", "us/call");
    for (int i = 0; i < DX_N; i++){
        DxStat *s = &g_dx[i];
        if (!s->n) continue;
        double ns = (double)s->ticks * tb.numer / tb.denom;
        fprintf(stderr, "  %-20s %10.1f   sub_%08x %10.3f %10.3f\n", s->name,
                (double)s->n / fr, s->addr, ns / 1e6 / fr, ns / 1e3 / s->n);
    }
    if (an_calls){
        fprintf(stderr, "  --- D3DXANALYZE: expression evaluation inputs ---\n");
        fprintf(stderr, "  calls %llu | first %llu | inputs SAME %llu (%.1f%%) | DIFFERENT %llu (%.1f%%)\n",
            (unsigned long long)an_calls, (unsigned long long)an_first,
            (unsigned long long)an_same, 100.0*an_same/an_calls,
            (unsigned long long)an_diff, 100.0*an_diff/an_calls);
        fprintf(stderr, "  without dirty inputs %llu (%.1f%%), avg hashed bytes %.0f\n",
            (unsigned long long)an_dirty0, 100.0*an_dirty0/an_calls, (double)an_bytes/an_calls);
        fprintf(stderr, "  dirty input count distribution:");
        for (int i = 0; i < 9; i++) fprintf(stderr, " %d:%llu", i, (unsigned long long)an_hist_inputs[i]);
        fprintf(stderr, "\n");
    }
}

