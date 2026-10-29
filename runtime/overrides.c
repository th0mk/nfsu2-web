/* Runtime wrappers for lifted functions.
 *
 * The D3DX effect parameter tree was being parsed CYCLICALLY (node 0 -> 6 -> 4..8 -> 6)
 * and one past the node array's capacity (8) was requested (index 8).
 * The root cause is still open. There are two guards here:
 *   1) array bounds: return safely on an index past the capacity
 *   2) visited set: each node at most once per top-level walk
 * That way the walk terminates and the game can make progress.            */
#include "cpu.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "enhance.h"

void guest_backtrace(CPU*,int);

uint64_t g_cycle_break = 0, g_oob_node = 0, g_depth_guard = 0;
static uint64_t guard_total = 0, guard_cap = 0;
static CPU *g_guard_cpu = 0;

#ifdef __wasm__
#define HEAP_LO 0x08000000u
#define HEAP_HI 0x28000000u
#else
#define HEAP_LO 0x10000000u
#define HEAP_HI 0x2F000000u
#endif

/* Find the heap block that contains a guest address (header: +0 size, +4 signature) */
static int block_of(uint32_t addr, uint32_t *ubeg, uint32_t *uend){
    if (addr < HEAP_LO || addr >= HEAP_HI) return 0;
    for (uint32_t p = (addr & ~15u); p >= HEAP_LO; p -= 16u){
        if (!mem_is_committed(p+4)) return 0;
        if (MR32(p+4) == 0xA110C8EDu){
            uint32_t sz = MR32(p), beg = p + 16u, end = beg + sz;
            if (addr >= beg && addr < end){ *ubeg = beg; *uend = end; return 1; }
            return 0;
        }
        if (p < 16u) break;
    }
    return 0;
}

static void guard_hit(void){
    if (!guard_cap){ const char *e = getenv("GUARD_CAP");
                     guard_cap = e ? (uint64_t)atoll(e) : 2000000ull; }
    if (guard_total == 5000 && g_guard_cpu && getenv("GUARD_BT")){
        fprintf(stderr,"\n=== call chain at the 5000th guard hit ===\n");
        guest_backtrace(g_guard_cpu, 14);
    }
    if (++guard_total > guard_cap){
        fprintf(stderr,"\n*** GUARD CEILING (%llu) - the effect system cannot make progress.\n",
                (unsigned long long)guard_cap);
        { extern uint32_t g_watch_addr; extern void watch_dump(const char*,int);
          if (g_watch_addr) watch_dump("guard ceiling", 20); }
        { extern void (*g_report_fn)(void); if (g_report_fn) g_report_fn(); }
        _exit(6);
    }
}

/* --- shared: node array bounds + visit check ------------------------- */
typedef struct { uint64_t bits[8]; uint32_t tab; } VisSet;   /* 512 nodes */

/* The tree guard is NOW OFF BY DEFAULT.
 * The cyclic parse actually came from CPU emulation bugs (x87 rounding,
 * SSE width, adc/sbb carry...).  With those fixed the walk terminates on
 * its own; the guard is only turned on with GUARD_ON=1, and then it also
 * cuts valid nodes and breaks the tree.                                  */
static int g_guard_off = -1;
static int guard_off(void){
    if (g_guard_off < 0) g_guard_off = getenv("GUARD_ON") ? 0 : 1;
    return g_guard_off;
}

static int node_guard(CPU *c, uint32_t a1, int depth, VisSet *vs){
    if (guard_off()) return 0;
    uint32_t tab = c->ecx + 0x2cu;
    if (!mem_is_committed(tab)) return 0;
    uint32_t arr = MR32(tab), beg, end;
    if (block_of(arr, &beg, &end)){
        uint32_t cap = (end - arr) / 0x64u;
        if (a1 >= cap){ g_oob_node++; guard_hit(); return 1; }
    }
    if (depth == 0){                       /* top level: reset the set */
        for (int i=0;i<8;i++) vs->bits[i]=0;
        vs->tab = arr;
    }
    if (arr == vs->tab && a1 < 512u){
        uint64_t *w = &vs->bits[a1 >> 6]; uint64_t b = 1ull << (a1 & 63);
        if (*w & b){ g_cycle_break++; guard_hit(); return 1; }
        *w |= b;
    }
    return 0;
}

/* --- sub_0066aa2f(a1, a2) : the main tree walker, ret 8, HRESULT ----- */
void sub_0066aa2f_orig(CPU *c);
static _Thread_local int d_aa3f = 0;
static _Thread_local VisSet vs_aa3f;

/* Dump the effect parameter tree's node array (diagnostic).
 * Node: 0x64 bytes.  +0x04 array base, +0x18/+0x1a child node indices
 * (0xFFFF = none), +0x58 type tag.                                       */
static void dump_nodes(CPU *c){
    uint32_t tab = c->ecx + 0x2cu;
    if (!mem_is_committed(tab)) return;
    uint32_t arr = MR32(tab), beg, end;
    if (!block_of(arr, &beg, &end)){ fprintf(stderr,"  [tree] array 0x%08x outside any block\n", arr); return; }
    uint32_t cap = (end - arr) / 0x64u;
    fprintf(stderr,"  [tree] array=0x%08x capacity=%u (block %08x..%08x)\n", arr, cap, beg, end);
    /* raw dump: to understand the node layout */
    for (uint32_t i = 0; i < cap && i < 4; i++){
        uint32_t n = arr + i*0x64u;
        if (!mem_is_committed(n+0x63)) break;
        fprintf(stderr,"    node[%u] raw @%08x:\n", i, n);
        for (int rw = 0; rw < 0x64; rw += 16){
            fprintf(stderr,"      +%02x:", rw);
            for (int q = 0; q < 16 && rw+q < 0x64; q++)
                fprintf(stderr," %02x", MR8(n+rw+q));
            fprintf(stderr,"   ");
            for (int q = 0; q < 16 && rw+q < 0x64; q++){
                uint8_t ch = MR8(n+rw+q);
                fputc((ch>=32&&ch<127)?ch:'.', stderr);
            }
            fprintf(stderr,"\n");
        }
        /* where do +0 and +4 point? */
        for (int f = 0; f <= 4; f += 4){
            uint32_t ptr = MR32(n+f);
            if (ptr && mem_is_committed(ptr+31)){
                fprintf(stderr,"      [+%d]->%08x:", f, ptr);
                for (int q=0;q<24;q++) fprintf(stderr," %02x", MR8(ptr+q));
                fprintf(stderr,"   ");
                for (int q=0;q<24;q++){ uint8_t ch=MR8(ptr+q);
                    fputc((ch>=32&&ch<127)?ch:'.', stderr); }
                fprintf(stderr,"\n");
            }
        }
    }
    for (uint32_t i = 0; i < cap && i < 64; i++){
        uint32_t n = arr + i*0x64u;
        if (!mem_is_committed(n+0x5C)) break;
        uint32_t nameptr = MR32(n);
        char nm[40]; nm[0]=0;
        if (nameptr && mem_is_committed(nameptr)){
            for (int k=0;k<32;k++){ uint8_t ch=MR8(nameptr+k);
                if (!ch || ch<32 || ch>126){ nm[k]=0; break; } nm[k]=(char)ch; nm[k+1]=0; }
        }
        fprintf(stderr,"    [%2u] name=0x%08x '%s' base=0x%08x sub18=%5u sub1a=%5u type=0x%08x\n",
                i, nameptr, nm, MR32(n+4),
                MR16(n+0x18), MR16(n+0x1a), MR32(n+0x58));
    }
}

/* Recursion chain: (node index, array base) pairs.
 * Dumped once the depth threshold is passed, to see the cycle concretely. */
static _Thread_local uint32_t chain_idx[160], chain_arr[160];
static int chain_dumped = 0;

void sub_0066aa2f(CPU *c){
    uint32_t a1 = MR32(c->esp + 4u);
    g_guard_cpu = c;
    { static int dumped = 0;
      if (!dumped && getenv("TREEDUMP")){ dumped = 1; dump_nodes(c); } }
    if (getenv("WALKLOG")){
        static int nlog = 0;
        if (nlog < 60){ nlog++;
            uint32_t tabp = c->ecx + 0x2cu;
            uint32_t arr = mem_is_committed(tabp) ? MR32(tabp) : 0;
            uint32_t esi = arr + a1*0x64u;
            if (mem_is_committed(esi+0x35))
                fprintf(stderr,"    [walk] depth=%2d a1=%-5u node=%08x "
                        "sub18=%5u sub1a=%5u count=%u base=%08x\n",
                        d_aa3f, a1, esi, MR16(esi+0x18), MR16(esi+0x1a),
                        MR16(esi+0x34), MR32(esi+4));
            else
                fprintf(stderr,"    [walk] depth=%2d a1=%-5u node=%08x UNREADABLE\n",
                        d_aa3f, a1, esi);
        }
    }
    if (d_aa3f < 160){ chain_idx[d_aa3f] = a1;
        chain_arr[d_aa3f] = mem_is_committed(c->ecx+0x2c) ? MR32(c->ecx+0x2c) : 0; }
    if (!chain_dumped && d_aa3f >= 40 && getenv("CHAIN")){
        chain_dumped = 1;
        fprintf(stderr,"  [chain] recursion depth %d, (index@array):\n", d_aa3f);
        for (int q = 0; q <= d_aa3f && q < 160; q++){
            fprintf(stderr," %u@%08x", chain_idx[q], chain_arr[q]);
            if ((q % 8) == 7) fprintf(stderr,"\n");
        }
        fprintf(stderr,"\n");
    }
    if (node_guard(c, a1, d_aa3f, &vs_aa3f)){
        POP32(c->eip); c->esp += 8u; c->eax = 0;   /* S_OK */
        return;
    }
    if (d_aa3f >= 128){ g_depth_guard++; guard_hit();
        POP32(c->eip); c->esp += 8u; c->eax = 0; return; }
    d_aa3f++;
    sub_0066aa2f_orig(c);
    d_aa3f--;
}

/* --- sub_0065ae25(a1, a2, a3) : helper walker, ret 0xc -------------- */
void sub_0065ae25_orig(CPU *c);
static _Thread_local int d_ae35 = 0;
static _Thread_local VisSet vs_ae35;

void sub_0065ae25(CPU *c){
    uint32_t a1 = MR32(c->esp + 4u);
    g_guard_cpu = c;
    if (node_guard(c, a1, d_ae35, &vs_ae35)){
        POP32(c->eip); c->esp += 12u; c->eax = 0;
        return;
    }
    if (d_ae35 >= 128){ g_depth_guard++; guard_hit();
        POP32(c->eip); c->esp += 12u; c->eax = 0; return; }
    d_ae35++;
    sub_0065ae25_orig(c);
    d_ae35--;
}

/* --- sub_006e2be0(list, b, c): walks the list and frees every item ------
 * The first argument can arrive NULL (a case that does not occur on Windows).
 * The original code has no null check and crashes; we return safely.     */
void sub_006e2be0(CPU *c);                     /* generated original */
uint64_t g_null_list = 0, g_e2be0_calls = 0;
static void wrap_6e2be0(CPU *c){
    uint32_t a1 = MR32(c->esp + 4u);
    g_e2be0_calls++;
    if (a1 == 0 || !mem_is_committed(a1 + 4u)){
        g_null_list++;
        POP32(c->eip);                          /* ret (cdecl: caller cleans up) */
        return;
    }
    sub_006e2be0(c);
}


/* --- sub_006623da: copies an effect parameter value into the shader constant block ---
 * __thiscall, 7 stack arguments.  Stack layout (caller 0x00669700):
 *   [esp+04] a1  -> *(a1)=source data, *(a1+4)=type descriptor
 *   [esp+08] a2  = destination (inside the constant block)
 *   [esp+0c] a3  = D3DXPARAMETER_TYPE (1=BOOL 2=INT 3=FLOAT)
 *   [esp+10] a4  = row count
 *   [esp+14] a5  = column count
 * Input/output is dumped to see why the WVP matrix was written incompletely. */
void sub_006623da_orig(CPU *c);
uint64_t g_copy_calls = 0;

void sub_006623da(CPU *c){
    uint32_t a1 = MR32(c->esp + 4u),  dst = MR32(c->esp + 8u);
    uint32_t ty = MR32(c->esp + 12u), rows = MR32(c->esp + 16u);
    uint32_t cols = MR32(c->esp + 20u);
    uint32_t src = (a1 && mem_is_committed(a1+7)) ? MR32(a1) : 0;
    uint32_t td  = (a1 && mem_is_committed(a1+7)) ? MR32(a1+4) : 0;
    uint32_t cls = (td && mem_is_committed(td+7)) ? MR32(td+4) : 0xFFFFFFFFu;
    g_copy_calls++;
    /* the FE effect's constant block is 80 bytes; trace only copies writing to it */
    static int n = 0;
    int is80 = 0;
    { uint32_t b1, e1;
      if (block_of(dst, &b1, &e1) && (e1 - b1) == 80u) is80 = 1; }
    int show = 0;   /* decided AFTER the copy */
    (void)show;
    /* MPROTSRC=1: WHO writes the FE effect's WVP value field?
     * Once the copy source is known we make the page read-only and catch it. */
    if (is80 && src && getenv("MPROTSRC")){
        static int armed3 = 0;
        uint32_t b2, e2;
        if (!armed3 && block_of(dst, &b2, &e2) && dst == b2){
            armed3 = 1;
            void mprot_arm(uint32_t);
            fprintf(stderr,"  [copy] WVP source %08x -- page write-protected\n", src);
            mprot_arm(src);
        }
    }
    sub_006623da_orig(c);
    /* Dump only the copies that write a MEANINGFUL value into the 80-byte FE block */
    if (getenv("COPYLOG") && is80 && n < 14 && dst && mem_is_committed(dst+63)){
        int meaningful = 0;
        for (int q=0;q<16;q++){
            float fv = MRF32(dst + 4*q);
            if (fv != 0.f && fabsf(fv) > 1e-6f && fabsf(fv) < 1e6f) meaningful = 1;
        }
        if (meaningful){ n++;
            uint32_t bb, ee; block_of(dst, &bb, &ee);
            fprintf(stderr,"  [copy %llu] a2=%u class=%d source=%08x target=%08x"
                    " (block %08x offset %d)\n",
                    (unsigned long long)g_copy_calls, ty, (int)cls, src, dst,
                    bb, (int)(dst - bb));
            if (src && mem_is_committed(src+63)){
                fprintf(stderr,"      source:");
                for (int r2=0;r2<4;r2++)
                    fprintf(stderr," [%.4g %.4g %.4g %.4g]", MRF32(src+r2*16),
                            MRF32(src+r2*16+4), MRF32(src+r2*16+8), MRF32(src+r2*16+12));
                fprintf(stderr,"\n");
            }
            fprintf(stderr,"      target:");
            for (int r2=0;r2<4;r2++)
                fprintf(stderr," [%.4g %.4g %.4g %.4g]", MRF32(dst+r2*16),
                        MRF32(dst+r2*16+4), MRF32(dst+r2*16+8), MRF32(dst+r2*16+12));
            fprintf(stderr,"\n");
            guest_backtrace(c, 8);
        }
    }
}

void overrides_install(void){
    void dispatch_patch(uint32_t, void(*)(CPU*));
    dispatch_patch(0x006e2be0u, wrap_6e2be0);
    void d3dx_hle_install(void);
    d3dx_hle_install();
}

static uint64_t g_jdlz_n, g_jdlz_bytes, g_jdlz_lifted, g_pm_hits, g_pm_calls, g_pm_clears;
void overrides_report(void){
    if (g_pm_calls)
        fprintf(stderr,"  NewGetCarPart: %llu of %llu answers kept from before (all forgotten %llu times)\n", (unsigned long long)g_pm_hits,
                (unsigned long long)g_pm_calls, (unsigned long long)g_pm_clears);
    if (g_jdlz_n || g_jdlz_lifted)
        fprintf(stderr,"  JDLZ: %llu blocks unpacked natively (%.1f MB), %llu by the game's code\n",
                (unsigned long long)g_jdlz_n, g_jdlz_bytes / 1048576.0, (unsigned long long)g_jdlz_lifted);
    if (g_e2be0_calls)
        fprintf(stderr,"  sub_006e2be0: %llu calls, %llu NULL lists skipped\n",
                (unsigned long long)g_e2be0_calls,(unsigned long long)g_null_list);
    if (g_cycle_break || g_oob_node || g_depth_guard)
        fprintf(stderr,"  tree guard: cycles %llu, out of bounds %llu, depth %llu\n",
                (unsigned long long)g_cycle_break,
                (unsigned long long)g_oob_node,
                (unsigned long long)g_depth_guard);
}

/* --- sub_0050d510: the game's DEBUG printf ------------------------------
 * In the retail build it is stubbed out as "xor eax,eax; ret", so the
 * engine's whole internal log is thrown away: front-end package switches,
 * the message queue, "Joy Event" input dispatch...  We catch and print it here.
 * cdecl, variadic: format = [esp+4], arguments from [esp+8].           */
uint32_t g_fe_mgr = 0;   /* front-end package manager (captured) */
void sub_0050d510_orig(CPU *c);

static const char *gstr(uint32_t p){
    if (!p || !mem_is_committed(p)) return "(null)";
    return (const char *)(g_mem + p);
}
void sub_0050d510(CPU *c){
    /* v12na: screen tracking (autotest waitfng/tapuntil) -- independent of GAMELOG.
     * The current front-end screen's name is taken from the "Init Package %s"
     * and "SwitchFEngPackages from %s to %s" calls. */
    { uint32_t fp = MR32(c->esp + 4u);
      if (fp && mem_is_committed(fp + 31)){
          const char *f = (const char *)(g_mem + fp);
          extern void autotest_note_screen(const char *);
          if (!strncmp(f, "Init Package %s", 15)) autotest_note_screen(gstr(MR32(c->esp + 8u)));
          else if (!strncmp(f, "SwitchFEngPackages from %s to %s", 32))
              autotest_note_screen(gstr(MR32(c->esp + 12u)));
      } }
    static int on = -1;
    if (on < 0){ const char *e = getenv("GAMELOG"); on = (e && *e && *e != '0'); }
    if (on){
        uint32_t fp = MR32(c->esp + 4u);
        if (fp && mem_is_committed(fp)){
            const char *f = (const char *)(g_mem + fp);
            uint32_t ap = c->esp + 8u;          /* next varargs slot */
            char out[1024]; size_t o = 0;
            for (const char *q = f; *q && o < sizeof out - 8; q++){
                if (*q != '%'){ out[o++] = *q; continue; }
                q++;
                if (*q == '%'){ out[o++] = '%'; continue; }
                /* skip the flag/width/precision fields */
                while (*q && (strchr("-+ #0", *q) || (*q >= '0' && *q <= '9') ||
                              *q == '.' || *q == 'l' || *q == 'h')) q++;
                char cv = *q;
                if (!cv) break;
                char tmp[256];
                if (cv == 's'){
                    uint32_t sp = mem_is_committed(ap+3) ? MR32(ap) : 0; ap += 4;
                    snprintf(tmp, sizeof tmp, "%s", gstr(sp));
                } else if (cv == 'c'){
                    uint32_t v = mem_is_committed(ap+3) ? MR32(ap) : 0; ap += 4;
                    snprintf(tmp, sizeof tmp, "%c", (int)(v & 0xFFu));
                } else if (cv == 'f' || cv == 'g' || cv == 'e'){
                    uint64_t lo = mem_is_committed(ap+7) ? ((uint64_t)MR32(ap) |
                                  ((uint64_t)MR32(ap+4) << 32)) : 0; ap += 8;
                    double dv; memcpy(&dv, &lo, 8);
                    snprintf(tmp, sizeof tmp, "%g", dv);
                } else {
                    uint32_t v = mem_is_committed(ap+3) ? MR32(ap) : 0; ap += 4;
                    if (cv == 'x') snprintf(tmp, sizeof tmp, "%x", v);
                    else if (cv == 'X') snprintf(tmp, sizeof tmp, "%X", v);
                    else if (cv == 'u') snprintf(tmp, sizeof tmp, "%u", v);
                    else if (cv == 'p') snprintf(tmp, sizeof tmp, "%08x", v);
                    else snprintf(tmp, sizeof tmp, "%d", (int)v);
                }
                for (const char *t2 = tmp; *t2 && o < sizeof out - 2; t2++) out[o++] = *t2;
            }
            out[o] = 0;
            fprintf(stderr, "  [game] %s%s", out,
                    (o && out[o-1] == '\n') ? "" : "\n");
            /* Inside SwitchFEngPackages ebx = the front-end package manager
             * ("this").  printf is called right at that point, so we catch the
             * pointer here for free; it is needed to force a switch. */
            if (!strncmp(out, "SwitchFEngPackages", 18)) g_fe_mgr = c->ebx;
            /* WHO triggers the screen switch?  The call chain of a successful
             * switch shows what is missing at the splash screen. */
            { const char *bt = getenv("GAMELOG_BT");
              static int nbt = 0;
              if (bt && *bt && nbt < 4 && !strncmp(out, bt, strlen(bt))){
                  nbt++; guest_backtrace(c, 16); } }
        }
    }
    sub_0050d510_orig(c);
}


/* --- Force rain (FORCERAIN) -----------------------------------------------
 * The game picks the weather at random at race start (measured: rain in ~1
 * of 8 runs); debugging a rain-specific problem needs reproducible rain.
 * Rain::Update = sub_0061f120 (__thiscall, ecx = the Rain* at eView+0x64):
 *   Rain+0x5094 target precipitation (0..1), +0x1f0 current intensity (moves
 *   toward the target), +0x509c cloud intensity.  The target is computed every
 *   frame by sub_0061e380 (if the "Rain" zone object 0x8a1f28 is active);
 *   otherwise the old target stays.
 * FORCERAIN=1 sets the target to 1.0, FORCERAIN=2 sets it to 0 (always dry).
 * FORCERAIN=3 OVERCAST sky without rain (+0x509c and its target +0x5098 = 0;
 * 1.0 = clear sky), FORCERAIN=4 overcast sky + rain.
 * Without the variable the behaviour is EXACTLY the game's.  In both builds.
 * The update itself goes through framerate.c (F6: it runs every frame above
 * 60 fps). */
static int force_rain(void){
    static int fr = -1;
    if (fr < 0){ const char *e = getenv("FORCERAIN"); fr = e ? atoi(e) : 0; }
    return fr;
}
void sub_0061e380_orig(CPU *c);
void sub_0061e380(CPU *c){
    sub_0061e380_orig(c);
    int fr = force_rain();
    if (fr == 1 || fr == 4) ST(0) = 1.0; else if (fr == 2 || fr == 3) ST(0) = 0.0;
}
void hfr_rain_update(CPU *c);          /* framerate.c: sub_0061f120_orig at any frame rate */
void sub_0061f120(CPU *c){
    uint32_t rain = c->ecx;
    int fr = force_rain();
    if (fr && rain && mem_is_committed(rain + 0x50a3u))
        MRF32(rain + 0x5094u) = (fr == 1 || fr == 4) ? 1.0f : 0.0f;
    hfr_rain_update(c);
    if (fr >= 3 && rain && mem_is_committed(rain + 0x50a3u)){
        MRF32(rain + 0x5098u) = 0.0f; MRF32(rain + 0x509cu) = 0.0f; }
}

/* --- Force the sky transition (FORCETOD) ----------------------------------
 * sub_0060faa0 = time-of-day / sky state machine (every frame while the game
 * state is 6).  A 300 s cycle on the world timer 0x7fb718 (s):
 * if (int)timer % 300 == 60 and state 0x8a1c8c == 0 it moves to state 2 and
 * the sky transition (skymorph.efx: sunset -> night) starts.  Test runs last
 * ~60 s, so they only hit this transition now and then (measured: ~1 in 8
 * runs), and the HD build got very slow when they did.
 * FORCETOD=1 triggers the transition at the first chance; FORCETOD=<frame count>
 * after that many calls.  Unset, the behaviour is EXACTLY the game's.  In both builds. */
void sub_0060faa0_orig(CPU *c);
void sub_0060faa0(CPU *c){
    static int ft = -1, done = 0; static long calls = 0;
    if (ft < 0){ const char *e = getenv("FORCETOD"); ft = e ? atoi(e) : 0; }
    if (ft > 0 && !done && MR32(0x8654a4u) == 6u && MR32(0x8a1c78u) != 0u && ++calls >= ft &&
        MR32(0x8a1c8cu) == 0u){
        static int st = -1;
        if (st < 0){ const char *e = getenv("FORCETOD_STATE"); st = e ? atoi(e) : 2; if (st < 1 || st > 4) st = 2; }
        MR32(0x8a1c8cu) = (uint32_t)st; done = 1;
        fprintf(stderr, "[FORCETOD] sky state forced to %d (call %ld)\n", st, calls);
    }
    sub_0060faa0_orig(c);
}

/* --- sub_005c7ec0: per-view camera/projection setup ---------------------
 * __thiscall(ecx = matrix struct, [esp+4] = eView*), ret 0x10.
 *   eView+0x04 = view id (1 main, 3 rear-view mirror, 4 road reflection)
 *   eView+0x40 = Camera*  (CameraParams: +0xbc NearZ, +0xc0 FarZ, +0xc4 FOV)
 *   ecx+0x00 view matrix, +0x40 projection, +0x80 their product
 * The game only passes identity matrices to SetTransform; the camera can only
 * be read here.  The address is captured and (in the HD build) the main
 * view's matrices are recorded into the frame.  In the original build
 * enh_view_setup is an empty inline function.  The web build keeps the
 * headlight view's (9) for the other cars' lights (headlights.c), and turns
 * the view against a phone's tilt (below). */
#ifdef __wasm__
/* The touch controls' level horizon (web/src/app/touch.ts): while tilt
 * steers, the main view (1) and its road reflection (4, laid over it in
 * screen space) turn against the phone's tilt, about the camera's forward
 * axis, so the road's horizon stays level with the real one; the HUD stays
 * on the phone.  The camera's matrix is turned for the length of the call:
 * the view matrix copied from it (0x4012b0), the product and the culling
 * planes made of that agree, and whatever reads the camera itself keeps the
 * game's.  The game's view space has y down (the projection's P11 is
 * negative), so this turns the picture anticlockwise for a + roll.  Photo
 * mode's tilt turns it the same way, and its field of view (camera +0xC4, a
 * binary angle) stands in for the camera's for the call (photo.c). */
float input_view_roll(void);
int photo_view(uint16_t *fov, float *roll);
static void view_roll(uint32_t m, float a){
    float c = cosf(a), s = sinf(a);
    for (uint32_t r = m; r < m + 0x40u; r += 0x10u){
        float x = MRF32(r), y = MRF32(r + 4u);
        MRF32(r) = x * c + y * s;
        MRF32(r + 4u) = y * c - x * s;
    }
}
#endif
void sub_005c7ec0_orig(CPU *c);
void sub_005c7ec0(CPU *c){
    uint32_t plat = c->ecx, view = MR32(c->esp + 4u);
#ifdef __wasm__
    uint32_t cam = 0, fcam = 0; uint8_t keep[0x40]; uint16_t fov = 0, fkeep = 0;
    if (view && mem_is_committed(view + 0x43u) && (MR32(view + 4u) == 1u || MR32(view + 4u) == 4u)){
        float a = input_view_roll(), pr = 0.f;
        if (photo_view(&fov, &pr)){
            a += pr;
            fcam = MR32(view + 0x40u);
            if (fcam && mem_is_committed(fcam + 0xc5u)){ fkeep = MR16(fcam + 0xc4u); MR16(fcam + 0xc4u) = fov; }
            else fcam = 0;
        }
        cam = a != 0.f ? MR32(view + 0x40u) : 0u;
        if (cam && mem_is_committed(cam + 0x3fu)){ memcpy(keep, g_mem + cam, sizeof keep); view_roll(cam, a); }
        else cam = 0;
    }
    sub_005c7ec0_orig(c);
    if (cam) memcpy(g_mem + cam, keep, sizeof keep);
    if (fcam) MR16(fcam + 0xc4u) = fkeep;
#else
    sub_005c7ec0_orig(c);
#endif
    if (view && plat && mem_is_committed(view + 0x43u) && mem_is_committed(plat + 0xbfu)){
        enh_view_setup(MR32(view + 4u), (const float *)(g_mem + plat),
                       MR32(view + 0x40u));
#ifdef __wasm__
        void headlights_view(uint32_t id, const float *plat);
        headlights_view(MR32(view + 4u), (const float *)(g_mem + plat));
#endif
    }
}


/* --- sub_0075bed4: the CRT's _ftol2 ---------------------------------------
 * ST(0) -> int64 in edx:eax, truncated toward zero; pops ST(0).  The hottest
 * lifted function on both ports (3.4 % of the Mac's race frame; 2.3 % in
 * Chrome).  It
 * rounds with fistp in the control word's RC, takes the difference (rounded
 * through X87R at PC=24), stores it as a float and adds or subtracts its sign
 * bit as a carry.  The wasm build runs this equivalent of the lifted body:
 * the same edx:eax, ecx, flags, x87 slots and top, esp and eip; only the
 * scratch stores below the caller's esp (its own frame) are not made.
 * tests/main_all.c section 6 checks it against the lifted body.  The Mac
 * keeps the lifted body (one forwarding call). */
void sub_0075bed4_orig(CPU *c);
void sub_0075bed4(CPU *restrict c){
#ifdef __wasm__
    FTOL2_BODY(c);                                          /* cpu.h */
    POP32(c->eip);
#else
    sub_0075bed4_orig(c);
#endif
}

/* --- the view-frustum box test ---------------------------------------------
 * sub_005bc210(M, lo, hi), cdecl: the box lo..hi (3 floats each, in and out)
 * through the matrix M (rows 0-2, translation in row 3), min and max per
 * axis summed term by term.  sub_005bc300(lo, hi, M), thiscall, ret 0xC: the
 * box (through M when M is not 0) against the six planes (n.x n.y n.z d) at
 * [ecx] + 0xC0: 0 outside one of them, 1 crossing one, 2 inside all.
 * sub_0048ac60 is a jmp to it that the lift copies whole.  The three were
 * ~6 % of a race frame in Chrome: run per object, with the x87 stack in
 * memory and every result tested.
 * On wasm they run in f32 here: at PC=24 and RC=nearest the x87 rounds each
 * result to 24 bits once, as f32 arithmetic does, while the result is a
 * normal float or zero (the lifted X87R gives the same: for float operands
 * the double result rounded to float is the correctly rounded one).  Every
 * result is checked for that; anything else (a NaN, an infinity, a float
 * subnormal, another control word) runs the lifted body from the untouched
 * state.  Same registers, flags, x87 top and condition bits, outputs and
 * stores at or above the caller's esp as the lifted bodies; not the scratch
 * stores below it, nor the x87 slots they leave empty.  tests/main_all.c
 * section 7 checks them against the lifted bodies.  The Mac keeps those. */
void sub_005bc210_orig(CPU *c);
void sub_005bc300_orig(CPU *c);
void sub_0048ac60_orig(CPU *c);
uint64_t g_cull_lifted;            /* calls that ran the lifted body (tests) */
#ifdef __wasm__
/* the x87 at PC=24 has this result too: a normal float or zero */
static inline uint32_t cull_ok(float x){
    float a = __builtin_fabsf(x);
    return (uint32_t)(x == 0.0f) | ((uint32_t)(a >= 0x1p-126f) & (uint32_t)(a <= 0x1.fffffep127f));
}
/* sub_005bc210's arithmetic on lo/hi (in place); 0 if the lifted body must
 * run.  *cmp: its last fcom (row 2, column 2: lo product vs hi product),
 * *pb: the hi product it stored in its first argument's slot then. */
static int cull_xform(uint32_t M, float lo[3], float hi[3], int *cmp, float *pb_last){
    float mn[3], mx[3], pa = 0, pb = 0;
    uint32_t ok = 1;
    for (int j = 0; j < 3; j++) mn[j] = mx[j] = MRF32(M + 0x30u + 4u * (uint32_t)j);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++){
            float m = MRF32(M + 16u * (uint32_t)i + 4u * (uint32_t)j);
            pa = lo[i] * m; pb = hi[i] * m;
            if (pa < pb){ mn[j] += pa; mx[j] += pb; } else { mn[j] += pb; mx[j] += pa; }
            ok &= cull_ok(pa) & cull_ok(pb) & cull_ok(mn[j]) & cull_ok(mx[j]);
        }
    if (!ok) return 0;
    *cmp = pa < pb ? -1 : pa > pb ? 1 : 0; *pb_last = pb;
    for (int j = 0; j < 3; j++){ lo[j] = mn[j]; hi[j] = mx[j]; }
    return 1;
}
static void cull_fsw(CPU *restrict c, int cmp){
    c->fsw &= (uint16_t)~(FSW_C0 | FSW_C2 | FSW_C3);
    if (cmp < 0) c->fsw |= FSW_C0; else if (cmp == 0) c->fsw |= FSW_C3;
}
static int xform_native(CPU *restrict c){
    if (c->fcw & 0x0F00u) return 0;
    uint32_t e = c->esp, M = MR32(e + 4u), A = MR32(e + 8u), B = MR32(e + 12u);
    float lo[3], hi[3]; int cmp; float pb;
    for (int j = 0; j < 3; j++){ lo[j] = MRF32(A + 4u * (uint32_t)j); hi[j] = MRF32(B + 4u * (uint32_t)j); }
    if (!cull_xform(M, lo, hi, &cmp, &pb)) return 0;
    for (int j = 0; j < 3; j++){ MRF32(A + 4u * (uint32_t)j) = lo[j]; MRF32(B + 4u * (uint32_t)j) = hi[j]; }
    MRF32(e + 4u) = pb;                                   /* its fstp [esp+0x20] scratch */
    cull_fsw(c, cmp);
    c->eax = (M & 0xffff0000u) | fsw_get_top(c, ((uint32_t)c->top - 7u) & 7u);   /* fnstsw ax */
    c->ecx = A + 12u; c->edx = M + 0x38u;
    { uint32_t a = e - 12u, r = e; FLAGS_ADD_M(a, 0xcu, r, 32, F_ALL); }         /* add esp, 0xc */
    POP32(c->eip);
    return 1;
}
static int frustum_native(CPU *restrict c){
    if (c->fcw & 0x0F00u) return 0;
    uint32_t e = c->esp, P1 = MR32(e + 4u), P2 = MR32(e + 8u), M = MR32(e + 12u);
    float lo[3], hi[3], C[3], H[3]; int cmp; float pb;
    for (int j = 0; j < 3; j++){ lo[j] = MRF32(P1 + 4u * (uint32_t)j); hi[j] = MRF32(P2 + 4u * (uint32_t)j); }
    if (M && !cull_xform(M, lo, hi, &cmp, &pb)) return 0;
    float half = MRF32(0x00784260u), K = MRF32(0x007d7d3cu);
    uint32_t ok = 1;
    for (int j = 0; j < 3; j++){
        float s = hi[j] + lo[j]; C[j] = s * half; H[j] = hi[j] - C[j];
        ok &= cull_ok(s) & cull_ok(C[j]) & cull_ok(H[j]);
    }
    if (!ok) return 0;
    uint32_t base = MR32(c->ecx), p = base + 0xc0u, inter = 0;
    int k, last = 0;
    for (k = 0; k < 6; k++, p += 16u){
        float nx = MRF32(p), ny = MRF32(p + 4u), nz = MRF32(p + 8u), d = MRF32(p + 12u);
        float r1 = __builtin_fabsf(nz) * H[2], r2 = __builtin_fabsf(nx) * H[0], r3 = __builtin_fabsf(ny) * H[1];
        float r12 = r1 + r2, r = r12 + r3;
        float s1 = C[2] * nz, s2 = C[0] * nx, s3 = C[1] * ny;
        float s12 = s1 + s2, s123 = s12 + s3, s = s123 + d, sp = s + r;
        if (!(cull_ok(r1) & cull_ok(r2) & cull_ok(r3) & cull_ok(r12) & cull_ok(r) & cull_ok(s1) & cull_ok(s2)
              & cull_ok(s3) & cull_ok(s12) & cull_ok(s123) & cull_ok(s) & cull_ok(sp))) return 0;
        if (sp < K) break;                                /* outside this plane */
        float sm = s - r;
        if (!cull_ok(sm)) return 0;
        if (sm < K) inter = 1;
        last = sm < K ? -1 : sm > K ? 1 : 0;
    }
    if (k < 6){                                           /* fcomp; ...; fstp st0; xor eax, eax */
        cull_fsw(c, -1);
        c->eax = 0; c->ecx = base + 0xc4u + 16u * (uint32_t)k; c->edx = (uint32_t)k + 1u;
        FLAGS_LOG_M(0u, 32, F_ALL);
    } else {                                              /* test esi, esi; sete al; inc eax */
        cull_fsw(c, last);
        uint32_t a = inter ? 0u : 1u, r = a + 1u;
        c->eax = r; c->ecx = base + 0xc4u + 0x60u; c->edx = 7u;
        FLAGS_LOG_M(inter, 32, F_ALL);
        { uint32_t cf = c->cf; FLAGS_ADD_M(a, 1u, r, 32, F_ALL); c->cf = cf; }
    }
    POP32(c->eip); c->esp += 12u;
    return 1;
}
#endif
void sub_005bc210(CPU *restrict c){
#ifdef __wasm__
    if (xform_native(c)) return;
#endif
    g_cull_lifted++;
    sub_005bc210_orig(c);
}
void sub_005bc300(CPU *restrict c){
#ifdef __wasm__
    if (frustum_native(c)) return;
#endif
    g_cull_lifted++;
    sub_005bc300_orig(c);
}
void sub_0048ac60(CPU *restrict c){
#ifdef __wasm__
    if (frustum_native(c)) return;
#endif
    g_cull_lifted++;
    sub_0048ac60_orig(c);
}

/* --- sub_0068ce7c: D3DXMatrixMultiply, the SSE version ---------------------
 * (out, a, b), stdcall: out = a * b, 4x4 floats, returns out.  D3DX calls it
 * through its CPU table for every matrix product: 2.7 % of a race frame in
 * Chrome, where the lift does each packed operation lane by lane through the
 * SSE NaN rule.  On wasm it runs here as f32x4 operations in the same order
 * (row 0 (a0 b0 + a1 b1) + (a2 b2 + a3 b3), rows 1 and 2 summed left to
 * right, row 3 as row 0): IEEE results like SSE's, denormals included (the
 * game leaves MXCSR at its default).  A NaN anywhere runs the lifted body
 * (wasm does not keep SSE's NaN operand order).  The same memory, eax, ecx,
 * edx and xmm0-7 as the lifted body; tests/main_all.c section 8 checks it.
 * The Mac keeps the lifted body. */
void sub_0068ce7c_orig(CPU *c);
uint64_t g_mmul_lifted;            /* calls that ran the lifted body (tests) */
void sub_0068ce7c(CPU *restrict c){
#ifdef __wasm__
#pragma clang fp contract(off)
    typedef float v4 __attribute__((vector_size(16), aligned(4)));
    uint32_t e = c->esp, out = MR32(e + 4u), A = MR32(e + 8u), B = MR32(e + 12u);
    v4 r0, r1, r2, r3; float a[16];
    memcpy(&r0, g_mem + B, 16); memcpy(&r1, g_mem + B + 16u, 16);
    memcpy(&r2, g_mem + B + 32u, 16); memcpy(&r3, g_mem + B + 48u, 16);
    memcpy(a, g_mem + A, 64);
#define BC(x_) ((v4){ (x_), (x_), (x_), (x_) })
    v4 o0 = (BC(a[0]) * r0 + BC(a[1]) * r1) + (BC(a[2]) * r2 + BC(a[3]) * r3);
    v4 o1 = ((BC(a[4]) * r0 + BC(a[5]) * r1) + BC(a[6]) * r2) + BC(a[7]) * r3;
    v4 o2 = ((BC(a[8]) * r0 + BC(a[9]) * r1) + BC(a[10]) * r2) + BC(a[11]) * r3;
    v4 p0 = r0 * BC(a[12]), p1 = r1 * BC(a[13]), p3 = r3 * BC(a[15]), q = r2 * BC(a[14]) + p3;
    v4 o3 = (p0 + p1) + q;
    __typeof__(o0 != o0) nan = (o0 != o0) | (o1 != o1) | (o2 != o2) | (o3 != o3);
    if (!(nan[0] | nan[1] | nan[2] | nan[3])){
        memcpy(g_mem + out, &o0, 16); memcpy(g_mem + out + 16u, &o1, 16);
        memcpy(g_mem + out + 32u, &o2, 16); memcpy(g_mem + out + 48u, &o3, 16);
        v4 x7 = BC(a[15]);
        memcpy(&c->xmm[0], &o3, 16); memcpy(&c->xmm[1], &p1, 16); memcpy(&c->xmm[2], &q, 16); memcpy(&c->xmm[3], &p3, 16);
        memcpy(&c->xmm[4], &o0, 16); memcpy(&c->xmm[5], &o1, 16); memcpy(&c->xmm[6], &o2, 16); memcpy(&c->xmm[7], &x7, 16);
        c->eax = out; c->ecx = B; c->edx = out;
        POP32(c->eip); c->esp += 12u;
        return;
    }
#undef BC
    g_mmul_lifted++;
#endif
    sub_0068ce7c_orig(c);
}

/* --- UI_ChoosePerformancePackage: a field the game never initialises ------
 * The Performance Shop's package screen (vtable 0x79D2D0, 0x250 bytes from
 * operator new, constructor sub_00559c90) never writes its field +0xC0, but
 * its refresh method sub_005620d0 -- first called from inside the
 * constructor (0x556E60 -> 0x557119) -- reads it: -10 or 1 there means
 * "close the parts list and open it again".  The field holds whatever the
 * heap block held before (0xEEEEEEEE in a fresh block; 1 after some heap
 * histories: movies played, 60 fps or a slow frame rate).  With 1, entering
 * a category whose carousel starts at "Choose Individual Parts" (Nitrous
 * Oxide) opens UI_BuyPerformanceParts twice; the first one is popped at once,
 * the package screen stays hidden with control mask 0 under the second, and
 * after ESC no screen takes input: the shop is dead until Cmd-Q
 * (+0xC0 = 1 at sub_005620d0's first call, 0xC3ED0000 in the good run;
 * the mouse position plays no part).  Nothing else in the game reads or writes the
 * field (all accesses in 0x540000..0x575000), so the screen is given the
 * value of a fresh block before its constructor runs: the parts list opens
 * once, both screens keep input, and ESC goes back to the categories, as in
 * every run where the stale value was not -10 or 1. */
void sub_00559c90_orig(CPU *c);
void sub_00559c90(CPU *c){
    uint32_t self = c->ecx;
    if (self && mem_is_committed(self) && mem_is_committed(self + 0xc3u))
        MR32(self + 0xc0u) = 0xEEEEEEEEu;
    sub_00559c90_orig(c);
}

/* --- sub_0050b410: FEng's level sorter, more than 512 objects a package ---
 * The FE renderer draws a package (the package loop sub_00536520) by
 * gathering its objects into a list of 1024 (renderer +0x114), sorting it,
 * and handing each object to the level sorter (sub_0051b850), which hangs it
 * in one of 256 depth buckets on an 8-byte node of the array sub_0050b410
 * (cdecl: package, nodes, bytes) gives it: 0x1000 bytes of the package
 * loop's stack, 512 nodes.  An object past them is not drawn.  The game's
 * own packages fit in that; free roam's world map does not (a marker for
 * each player in free roam besides the map's own 235 objects, roam_game.c),
 * and lost its shop and event icons to the markers.  Every
 * package gets 1024 nodes instead, as many as its list holds (roam_host.h
 * RH_FE_NODES); the packages are drawn one after another, and
 * sub_0050b430 lets go of the nodes after each. */
#define FE_NODES 1024u
void sub_0050b410_orig(CPU *c);
void sub_0050b410(CPU *c){
    uint32_t gheap_alloc(int heap, uint32_t n, int zero);
    static uint32_t nodes;
    sub_0050b410_orig(c);
    if (!nodes) nodes = gheap_alloc(1, FE_NODES * 8u, 1);
    if (nodes){ MR32(0x00838494u) = nodes; MR32(0x0083848cu) = FE_NODES; }
}

/* --- sub_004df9c0: a screen's list of the player's cars, a stock car of no type ---
 * The car lot's list (filter 1: the stock cars) and the game's other lists of
 * the player's cars (0x4ED935: filter 4) are made here (thiscall: the screen,
 * its +0xEC the player; ret 4) from the player's car records (FEDatabase
 * +0x3C0, 0x83AD90 + 0xAB4C a player: 91 pointers at +0x9BD8, 0x5162D0),
 * each one's car type looked up in CarTypeInfo[] ([0x8A1CCC], 46 of 0x890
 * bytes) with no bound.  The 48 stock cars (0x1C bytes from +0: +8 the
 * handle, bStringHash of "STOCK_<name>", +0x10 in use, +0x14 1, +0x18 the
 * type's index, which the vtable 0x79AADC's 0x503A30 returns) are made once,
 * for a new profile (0x516500: the types the car lot sells), then saved and
 * loaded with the profile as they are (0x5166B0, 0x516850).  A player's
 * profile held one in use with an index no car has: the car lot's list read
 * its CarTypeInfo out of bounds and the game ended as the car lot opened, so
 * the player could buy no car (2026-10-23 report on c53ecc4: memory access
 * out of bounds in 0x4DF9C0, at +0xDA; tests/scripts/career_lot_badstock.txt
 * makes the same trap).  How the profile got it is not known.  Before a list
 * is made, a stock car in use past the 46 types has the table made again as
 * the game makes it for a new profile, and the records that leaves unused get
 * the index of none (-1, as their constructor 0x503A00 leaves it); the
 * profile keeps the table at its next save. */
#define FECARS          0x0083ad90u
#define FECARS_PLAYER   0xab4cu
#define STOCK_N         48u
#define STOCK_SZ        0x1cu
#define CARTYPES_N      46u           /* the builder walks 0x189E0 bytes of CarTypeInfo */
void sub_004df9c0_orig(CPU *c);
void sub_00516500(CPU *c);
void sub_004df9c0(CPU *c){
    uint32_t who = MR32(c->ecx + 0xecu);
    if (who < 2u){
        uint32_t db = FECARS + who * FECARS_PLAYER, bad = STOCK_N;
        for (uint32_t i = 0; i < STOCK_N && bad == STOCK_N; i++){
            uint32_t r = db + i * STOCK_SZ;
            if (MR32(r + 0x10u) && MR32(r + 0x18u) >= CARTYPES_N) bad = i;
        }
        if (bad < STOCK_N){
            uint32_t r = db + bad * STOCK_SZ;
            fprintf(stderr, "[cars] player %u's stock car %u (handle 0x%08x) has car type %u, none of the game's %u: "
                    "the stock cars made again, as for a new profile\n", who, bad, MR32(r + 8u), MR32(r + 0x18u), CARTYPES_N);
            CPU save = *c;
            c->esp = (c->esp - 256u) & ~15u;
            PUSH32(0u);                           /* its argument, not read */
            c->ecx = db;
            PUSH32(0x004df9c0u);                  /* the ret pops it into eip; the state is put back below */
            sub_00516500(c);
            *c = save;
            for (uint32_t i = 0; i < STOCK_N; i++)
                if (!MR32(db + i * STOCK_SZ + 0x10u)) MR32(db + i * STOCK_SZ + 0x18u) = 0xffffffffu;
            fprintf(stderr, "[cars] %u stock cars\n", MR32(db + 0x540u));
        }
    }
    sub_004df9c0_orig(c);
}

/* --- sub_0073b900: the tick of the music's event system -------------------
 * The event system (the EA Trax player: tracks, pause, fades) guards its
 * state with a plain word, [0x8b7bc0]: the tick sets it while it runs, and
 * an API call that finds it set returns at once and does nothing (pause and
 * unpause 0x739030, and a dozen more in 0x738dd0..0x73b620).  The sound
 * thread runs the tick every 10 ms (0x721e13 -> 0x73b9c0), the game thread
 * too (0x483b40 -> 0x73b9b0).  On Windows a tick takes microseconds.  Here
 * the sound thread handed the GIL over inside it, at the sound library's
 * mutex (0x734e40, WaitForSingleObject) and at fair yields, and the game
 * thread then ran for a whole stretch with the word set.  Entering a shop
 * starts a new song held paused (a shop entry cut scene runs first) and later
 * unpauses it; in most runs that unpause came during such a stretch and was
 * dropped, so the song stayed paused: no more MusicSFX.mus reads, no music
 * in the shop or after it until the game was restarted.  The tick now keeps
 * the GIL (guest_noyield, win32_ext.c); a wait that blocks still releases
 * it. */
void sub_0073b900_orig(CPU *c);
void sub_0073b900(CPU *c){
    void guest_noyield(int);
    guest_noyield(+1);
    sub_0073b900_orig(c);
    guest_noyield(-1);
}

/* --- sub_00483e40: a positional sound's source goes ----------------------
 * The world's positional sounds (the list at [[0x82B884]+0x3C], vtable
 * 0x78BFD0) keep their sound objects at +0x24 and the frame's voice requests
 * at +4: bPNode list nodes, one per object waiting for a voice, their count
 * at +0xC.  A request recounts the list (0x479770).  The voice arbitration
 * (0x481850, from the list's update 0x481BD0) gives the requesters voices;
 * when the count and the voices in use [0x82B950] pass the limit [0x7ECBC8]
 * (5), it takes voices from quieter objects instead, walking its sorted copy
 * of the requests from the first node without checking that there is one
 * (0x481AB4).  Removing a source (0x483E40 -> 0x481E50, from the source's
 * destructor 0x5A9570) takes its object's request out of the list and
 * destroys the object, but leaves the count.  With every voice in use and
 * that the only request, the arbitration took its empty list's head for a
 * request, read an object pointer off the stack, and trapped (memory access
 * out of bounds) or called 0 (0x481B1C).  32 friends' cars round the player
 * keep all five voices busy, so requests wait, and a jump away (shopgo, the
 * world map's jump) frees the old place's world objects (0x5AEFA0) with
 * their sources.  After a removal the count is made again, as a request does
 * (0x43BE80: the list's length). */
void sub_00483e40_orig(CPU *c);
void sub_00483e40(CPU *c){
    uint32_t list = MR32(c->ecx + 0x3cu);
    sub_00483e40_orig(c);
    if (!MR32(0x7faaa8u) || !list) return;           /* sound off: nothing was removed */
    uint32_t head = list + 4u, n = 0;
    for (uint32_t e = MR32(head); e && e != head && n < 1024u; e = MR32(e)) n++;
    MR32(list + 0xcu) = n;
}

/* --- sub_0061c600: the car loader reads a car type's PARTS_ANIMATIONS.BIN ---
 * The CarLoader (0x8A3288) has one read under way at a time: a step that
 * starts one (a model or texture pack, a car's textures) sets its busy word
 * (+0x14, 0x8A329C), the read's callback (0x610A10, 0x610A30, 0x61CB40...)
 * clears it and runs the next step (0x63B160), and the frame's service
 * (0x63BCD0) steps only while it is clear.  This one (thiscall loader, the
 * car type's hash, ret 4) sets busy and starts its read without looking.
 * The step calls it only in the front end, after the car before it is done;
 * a cover shoot's start calls it at any time (0x4CE770 -> 0x633A00, the
 * player's car type), and in free roam a friend's car is often loading then
 * (in both shoots of the 2026-10-24 report).  Its callback (0x63BD10) then
 * cleared busy and stepped while the friend's read was still under way, so
 * the loader ran two reads at once and stepped a car on before its pack was
 * in, and busy could stay set for good.  The next Load that found the rides
 * pool full (0x63AE70: it waits for busy to clear, then purges) waited
 * forever, holding the GIL: the game froze with the sound looping, a few
 * friends' cars after the shoot or much later (the report: 1,000 frames
 * after it, in the middle of a friend's car load).
 * tests/scripts/roam_crowd_shoot.txt calls the shoot's start and
 * end while friends' cars load: hung in that wait after 24 of them, none in
 * 30 with this.  Here a read under way finishes first, the way that Load
 * waits (the reads serviced, 0x57EF60, until busy is clear).
 * NFSU2_TEST_LOADER_OVERLAP=1 (test aid): the game's way. */
#define CARLOADER_BUSY  0x008a329cu
#define CARTYPEINFO     0x008a1cccu   /* -> 46 x 0x890 bytes: +0 the name, +0xD0 the hash */
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);   /* com.c */
long autotest_frame(void);
void sub_0061c600_orig(CPU *c);
void sub_0061c600(CPU *c){
    static int overlap = -1;
    if (overlap < 0){ const char *e = getenv("NFSU2_TEST_LOADER_OVERLAP"); overlap = e && *e == '1'; }
    if (MR32(CARLOADER_BUSY) && overlap)
        fprintf(stderr, "[loader] f=%ld TEST: part animations read with the car loader's read under way\n", autotest_frame());
    if (MR32(CARLOADER_BUSY) && !overlap){
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        while (MR32(CARLOADER_BUSY)) call_guest(c, 0x0057ef60u, 0, 0);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        uint32_t h = MR32(c->esp + 4u), t = MR32(CARTYPEINFO);
        char nm[16] = "?";
        for (uint32_t k = 0; t && k < 46u; k++)
            if (MR32(t + k * 0x890u + 0xd0u) == h){
                for (int i = 0; i < 15 && MR8(t + k * 0x890u + (uint32_t)i); i++){ nm[i] = (char)MR8(t + k * 0x890u + (uint32_t)i); nm[i + 1] = 0; }
                break; }
        fprintf(stderr, "[loader] f=%ld %s's part animations waited %.0f ms for the car loader's read under way\n",
                autotest_frame(), nm, (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    }
    sub_0061c600_orig(c);
}

/* --- sub_00579810(read, priority, chunk): a queued read's priority, cdecl ---
 * The game's reads wait in one queue (0x57B4A0 puts one on 0x865220) and go
 * in order of priority, the lowest first (0x57E800 sorts the queue with
 * 0x5735C0 before it starts one), at most three at once and only those of
 * one file; the frame's service (0x57EF60, from the main loop at 0x5814C7)
 * finishes at most one of them.  Every read has priority 5 (0x57B410), the
 * track streamer's sections too: it sets 5 again here, with a 32 MB chunk
 * (0x5E61C0, 0x5E6230, the only callers), and keeps two of them under way.
 * In free roam the friends' cars load through the same queue (a car's
 * GEOMETRY.BIN alone is some 80 reads, merged 16 at a time), so a section
 * could wait behind whole cars.  When the streamer falls behind the player,
 * the city shows black holes and the world stops on its "Loading" screen
 * (HUD_World_Loading: 0x5DF810, the player less than 0.5 s from a section
 * that is not in): 20 s and more in a player's log of 2026-10-28 (every
 * block from the device's cache, 22 frames a second, twenty friends' cars
 * coming and going, 6-7 s each).  The sections go first now, 4 ahead of the
 * 5 of every other read.  Not shown to be the whole story: on a fast PC the
 * sections waited about as long either way (tests/scripts/
 * roam_crowd_stream.txt); the "[stream]" lines below tell, in a slow PC's
 * log, how long they wait.  NFSU2_TEST_STREAM_FIFO=1 (test aid): the game's
 * way. */
#define STREAM_READ_PRIORITY 4u
void sub_00579810_orig(CPU *c);
void sub_00579810(CPU *c){
    static int fifo = -1;
    if (fifo < 0){ const char *e = getenv("NFSU2_TEST_STREAM_FIFO"); fifo = e && *e == '1'; }
    if (!fifo && MR32(c->esp + 8u) == 5u) MR32(c->esp + 8u) = STREAM_READ_PRIORITY;
    sub_00579810_orig(c);
}
/* Every frame (roam.c's frame hook, before the main loop's service), for the
 * log: how long the streamer's sections waited, from their place in the
 * queue to their read's end, how many reads the queue held, and the world's
 * Loading screens.  A line a minute while sections are read
 * ("[stream] 60 s: ..."), and one at the end of each Loading screen (its
 * frames too: a game in the background runs almost none). */
#define READS_QUEUE      0x00865220u   /* reads waiting (list head): +0x5C callback, +0x6C id */
#define READS_STARTED    0x00865550u   /* reads under way (list head), the oldest first */
#define GAME_FLOW        0x008900d8u   /* -> the game's state machine: +0 state (1 loading), +8 its mode (2 in the world) */
static double ms_now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
void reads_frame(CPU *c){
    (void)c;
    /* the streamer's reads (callbacks 0x5DF110, a section, and 0x5E4F10, several) by id: seen this frame or done */
    static struct { uint32_t id; double t0; int seen; } sec[32];
    static int nsec, qmax, n, ld_n, ld_q, ld_sec;
    static long ld_f0;
    static double t_min, wsum, wmax, ld_t0, ld_s, ld_w;
    double now = ms_now();
    if (!t_min) t_min = now;
    for (int i = 0; i < nsec; i++) sec[i].seen = 0;
    int q = 0;
    for (int l = 0; l < 2; l++){
        uint32_t head = l ? READS_STARTED : READS_QUEUE, r = MR32(head);
        for (int k = 0; r != head && r && k < 512; k++, r = MR32(r)){
            if (!l) q++;
            uint32_t cb = MR32(r + 0x5cu), id = MR32(r + 0x6cu);
            if (cb != 0x005df110u && cb != 0x005e4f10u) continue;
            int i = 0;
            while (i < nsec && sec[i].id != id) i++;
            if (i == nsec && nsec < 32){ sec[nsec].id = id; sec[nsec].t0 = now; nsec++; }
            if (i < nsec) sec[i].seen = 1;
        }
    }
    if (q > qmax) qmax = q;
    if (q > ld_q) ld_q = q;
    for (int i = 0; i < nsec; ){
        if (sec[i].seen){ i++; continue; }
        double w = now - sec[i].t0;
        n++; wsum += w; if (w > wmax) wmax = w;
        ld_sec++; if (w > ld_w) ld_w = w;
        sec[i] = sec[--nsec];
    }
    uint32_t fl = MR32(GAME_FLOW);
    int loading = fl && MR32(fl) == 1u && MR32(fl + 8u) == 2u;
    if (loading && !ld_t0){ ld_t0 = now; ld_f0 = autotest_frame(); ld_sec = 0; ld_q = q; ld_w = 0; }
    if (!loading && ld_t0){
        double s = (now - ld_t0) / 1e3;
        ld_n++; ld_s += s; ld_t0 = 0;
        fprintf(stderr, "[stream] f=%ld the world waited %.1f s (%ld frames) on its Loading screen: %d section read(s) meanwhile, "
                "the longest wait %.0f ms, the game's queue up to %d reads\n", autotest_frame(), s, autotest_frame() - ld_f0, ld_sec, ld_w, ld_q);
    }
    if (now - t_min >= 60e3){
        if (n || ld_n)
            fprintf(stderr, "[stream] %.0f s: %d section read(s), each waited %.0f ms on average, the longest %.0f ms; "
                    "the game's queue up to %d reads; %d Loading screen(s), %.1f s\n",
                    (now - t_min) / 1e3, n, n ? wsum / n : 0.0, wmax, qmax, ld_n, ld_s);
        t_min = now; n = 0; wsum = wmax = 0; qmax = 0; ld_n = 0; ld_s = 0;
    }
}

/* --- sub_00631e20(list, ride, ...): a ride's texture names, cdecl -----------
 * A ride's textures by name hash, into two lists of 69 (list +0 and +0x114,
 * their counts at +0x228 and +0x22c): for the car loader's Load (0x63AE70,
 * the loader's copy of the ride, entry +0x20), which takes a reference to
 * each name on the loader's list of them (0x633C30, 0x8A32C0) and lets them
 * go when the ride is unloaded (0x63A000), and for a car's render info
 * (0x636840: the car's own ride, the player's that of their career).  Every
 * add skips a name the list has; a full list's add was meant to add nothing
 * but returns the count, so from the 70th different name on every add
 * doubles the count (0x610530 and some thirty copies of it inline).  A ride
 * with many different decals gets there: up to 56 (7 places of 8,
 * DECAL_HOOD_TEX0 .. DECAL_RIGHT_QUARTER_TEX7) after its other textures (a
 * 350Z with shop rims: 25).  With 56, the count was 69 x 2^12 = 282,624 and
 * Load took references for that many "names" read from past the list,
 * writing past it, and froze; with a few too many it corrupts less at once,
 * but the references written past the list are let go twice or never, and
 * the loader's list breaks: the 2026-10-24 report froze in a later load
 * walking it forever (0x633CA0), in free roam after an hour of friends' cars
 * coming and going (any player's car with more than about 44 different
 * decals).  Here a run without the ride's decals (the 7 places' shapes, ride
 * +0x660, off for it) counts the other textures, and the decals that would
 * not fit are off the ride while the list is made (the first ones, place by
 * place, stay), then back on it: the ride keeps them, its list has no names
 * for them.  It keeps locals in its arguments' places, so they are put back
 * for the second run.  tests/scripts/roam_crowd_decals.txt.
 * NFSU2_TEST_TEXLIST_FULL=1 (test aid): the game's way. */
#define TEXLIST_MAX     0x45u
#define RIDE_DECALZONES 0x660u        /* RideInfo: the 7 decal places' shapes (DECAL_HOOD ..), a part each */
#define RIDE_DECALS     0x714u        /* RideInfo: each place's 8 decals (+0x20 a place), a part each */
void sub_00631e20_orig(CPU *c);
void sub_00631e20(CPU *c){
    static int full = -1;
    if (full < 0){ const char *e = getenv("NFSU2_TEST_TEXLIST_FULL"); full = e && *e == '1'; }
    uint32_t esp0 = c->esp, list = MR32(esp0 + 4u), ride = MR32(esp0 + 8u);
    uint32_t seen[56], off[56], offp[56]; int nd = 0, noff = 0;
    for (uint32_t z = 0; z < 7u; z++){
        if (!MR32(ride + RIDE_DECALZONES + 4u * z)) continue;
        for (uint32_t i = 0; i < 8u; i++){
            uint32_t p = MR32(ride + RIDE_DECALS + 0x20u * z + 4u * i), k = 0;
            while (p && k < (uint32_t)nd && seen[k] != p) k++;
            if (p && k == (uint32_t)nd) seen[nd++] = p;
        }
    }
    if (full || !nd){ sub_00631e20_orig(c); return; }
    uint32_t arg[4], zone[7];
    for (uint32_t i = 0; i < 4u; i++) arg[i] = MR32(esp0 + 4u + 4u * i);
    for (uint32_t z = 0; z < 7u; z++){ zone[z] = MR32(ride + RIDE_DECALZONES + 4u * z); MR32(ride + RIDE_DECALZONES + 4u * z) = 0; }
    sub_00631e20_orig(c);
    for (uint32_t z = 0; z < 7u; z++) MR32(ride + RIDE_DECALZONES + 4u * z) = zone[z];
    uint32_t others = MR32(list + 0x228u);
    int room = others < TEXLIST_MAX ? (int)(TEXLIST_MAX - others) : 0, kept = 0;
    if (nd > room){
        for (uint32_t z = 0; z < 7u; z++){
            if (!zone[z]) continue;
            for (uint32_t i = 0; i < 8u; i++){
                uint32_t a = ride + RIDE_DECALS + 0x20u * z + 4u * i, p = MR32(a), k = 0;
                if (!p) continue;
                while (k < (uint32_t)kept && seen[k] != p) k++;
                if (k < (uint32_t)kept) continue;                    /* a decal already kept: no name more */
                if (kept < room) seen[kept++] = p;
                else { off[noff] = a; offp[noff++] = p; MR32(a) = 0; }
            }
        }
        fprintf(stderr, "[textures] f=%ld a ride with %d different decals and %u other textures: %d decal(s) left out of "
                "its texture list (it holds %u)\n", autotest_frame(), nd, others, nd - kept, TEXLIST_MAX);
    }
    c->esp = esp0;
    for (uint32_t i = 0; i < 4u; i++) MR32(esp0 + 4u + 4u * i) = arg[i];
    sub_00631e20_orig(c);
    for (int i = 0; i < noff; i++) MR32(off[i]) = offp[i];
    uint32_t n1 = MR32(list + 0x228u), n2 = MR32(list + 0x22cu);
    if (n1 > TEXLIST_MAX || n2 > TEXLIST_MAX){
        fprintf(stderr, "[textures] f=%ld a ride's texture lists still count %u and %u: cut to %u\n", autotest_frame(), n1, n2, TEXLIST_MAX);
        if (n1 > TEXLIST_MAX) MR32(list + 0x228u) = TEXLIST_MAX;
        if (n2 > TEXLIST_MAX) MR32(list + 0x22cu) = TEXLIST_MAX;
    }
}

/* --- sub_00639f20(texture set): a ride's textures let go, thiscall, ret 4 --
 * The car loader keeps a reference for each texture on a loaded ride's list
 * (its texture set, entry +0xCF0: the list +0x31C, the count +0x318) on the
 * texture's name record (the word at +0xC), and a texture is unloaded when
 * its count gets to 0.  This gives back the set's references, unloads the
 * textures that are then at 0 (0x637C10, 0x490150), and only then empties the
 * list and returns 1; when none got to 0 (each texture is on another ride's
 * list too: a car type shown twice) it returns 0 with the references given
 * back and the list as it was.  The purge (0x63A1B0 with phase 1: a load step
 * found no room in the Cars pool, 0x63AD97 and others) calls it for each ride
 * it holds, the ones on cars too, until one returns 1, and has the render
 * infos of that car type made again (0x5F4650).  The rides before it kept
 * their lists without references: their textures were unloaded when the
 * other ride went (Unload 0x63AE00 frees a ride nobody uses at once), with
 * the car still showing its ride, whose render info then drew with freed
 * textures -- its shadow (+0x33C, from 0x638827; 0x620AE0 -> 0x5CEA30) or a
 * part's (0x5C5930): "memory access out of bounds" in free roam with friends'
 * cars coming and going (the 2026-10-24 reports) -- and when the ride itself
 * went, its list gave the references back a second time.  Here a purge's call
 * that unloaded nothing takes them again; a ride's own unload (0x639FB0) lets
 * them go as before.  NFSU2_TEST_TEXREFS_DROP=1 (test aid): the game's way. */
#define PURGE_RET       0x0063a223u   /* the purge's call (phase 1) returns here */
#define TS_COUNT        0x318u
#define TS_NAMES        0x31cu
void sub_00639f20_orig(CPU *c);
void sub_00639f20(CPU *c){
    static int drop = -1; static long kept;
    if (drop < 0){ const char *e = getenv("NFSU2_TEST_TEXREFS_DROP"); drop = e && *e == '1'; }
    uint32_t ret = MR32(c->esp), ts = MR32(c->esp + 4u);
    sub_00639f20_orig(c);
    if (c->eax || ret != PURGE_RET || !ts || drop) return;
    uint32_t n = MR32(ts + TS_COUNT);
    if (n > TEXLIST_MAX) n = TEXLIST_MAX;
    if (!n) return;
    for (uint32_t i = 0; i < n; i++){
        uint32_t r = MR32(ts + TS_NAMES + 4u * i);
        if (r) MR16(r + 0xcu) = (uint16_t)(MR16(r + 0xcu) + 1u);
    }
    if (++kept == 1 || kept == 10 || kept == 100 || kept % 1000 == 0)
        fprintf(stderr, "[textures] f=%ld the car loader's purge gave back a ride's %u texture references and unloaded none: "
                "taken again (%ld times)\n", autotest_frame(), n, kept);
}

/* --- sub_00574670(block, out): a compressed block unpacked, cdecl ----------
 * The game's four kinds by the block's tag: COMP, HUFF, RAWW and JDLZ
 * (0x573700), for the model and texture packs' callbacks (0x48D090), the FE
 * packs (0x51BC81) and 0x577E46.  A car's TEXTURES.BIN keeps most of its
 * textures in JDLZ blocks (~8 MB unpacked), so each friend's car arriving in
 * free roam unpacked them on the game thread, lifted: 1.4 s of the game
 * thread's 97 among a crowd coming and going (tests/scripts/
 * roam_crowd_busy.txt, 656 cars), about 2 ms a car.  JDLZ is unpacked here,
 * natively, as the game's loop does it: the two flag words (a byte | 0x100,
 * shifted right, the next byte once down to 1); a flagged pair of bytes a
 * match -- with the second flag set 4 bits of distance - 1 and 12 of length
 * - 3, else 5 of length - 3 and 11 of distance - 17 -- copied a byte at a
 * time; a literal byte only while the output has room (past it the byte
 * stays unread but is counted); the loop runs until the block's size (+0xC)
 * less the 16-byte header and the first two flag bytes is used up; the
 * unpacked size (+8) is returned.  A block whose bytes are not all in guest
 * memory, which overlaps the output, or would read or write past the
 * output's ends goes to the game's code from the start (each byte it writes
 * again is the same).  The other kinds: the game's.  NFSU2_JDLZ_VERIFY=1
 * (test aid): each block unpacked both ways, compared ("[jdlz] DIFFERS"). */
#define JDLZ_TAG        0x5a4c444au
void sub_00574670_orig(CPU *c);
static int range_committed(uint32_t a, uint32_t n){
    if (!n) return 1;
    if (a + n < a) return 0;
    for (uint32_t pg = a >> 14, last = (a + n - 1u) >> 14; pg <= last; pg++)
        if (!((g_cbits[pg >> 3] >> (pg & 7)) & 1)) return 0;
    return 1;
}
/* 1: unpacked into out; 0: not done (out may hold some of it) */
static int jdlz_native(uint32_t blk, uint32_t out){
    uint32_t size = MR32(blk + 8u), csize = MR32(blk + 0xcu);
    if (csize < 0x12u || !range_committed(blk, csize) || !range_committed(out, size)) return 0;
    if (blk < out + size && out < blk + csize) return 0;
    const uint8_t *s = &MR8(blk + 0x12u);
    uint8_t *o0 = &MR8(out), *o = o0, *end = o0 + size;
    uint32_t f1 = (uint32_t)MR8(blk + 0x10u) | 0x100u, f2 = (uint32_t)MR8(blk + 0x11u) | 0x100u, left = csize - 0x12u;
    const uint8_t *s_end = &MR8(blk) + csize;
    while (left){
        if (f1 & 1u){
            uint32_t len, dist;
            if (s + 2 > s_end) return 0;
            if (f2 & 1u){ len = ((uint32_t)(s[0] & 0xf0u) << 4 | s[1]) + 3u; dist = (s[0] & 0x0fu) + 1u; }
            else { dist = ((uint32_t)(s[0] & 0xe0u) << 3 | s[1]) + 17u; len = (s[0] & 0x1fu) + 3u; }
            if ((uint32_t)(o - o0) < dist || len > (uint32_t)(end - o)) return 0;
            for (uint32_t i = 0; i < len; i++) o[i] = o[(int32_t)i - (int32_t)dist];
            o += len; s += 2; left -= 2u; f2 >>= 1;
            if (left > 0x80000000u) return 0;           /* (the game's count would have passed 0: it runs on) */
        } else {
            if (o < end){ if (s >= s_end) return 0; *o++ = *s++; }
            left--;
        }
        f1 >>= 1;
        if (f1 == 1u){ if (!left || s >= s_end) return 0; f1 = (uint32_t)*s++ | 0x100u; left--; }
        if (f2 == 1u){ if (!left || s >= s_end) return 0; f2 = (uint32_t)*s++ | 0x100u; left--; }
    }
    return 1;
}
void sub_00574670(CPU *restrict c){
    static int verify = -1;
    if (verify < 0){ const char *e = getenv("NFSU2_JDLZ_VERIFY"); verify = e && *e == '1'; }
    uint32_t e = c->esp, blk = MR32(e + 4u), out = MR32(e + 8u);
    if (blk && out && mem_is_committed(blk) && mem_is_committed(blk + 15u) && MR32(blk) == JDLZ_TAG && MR8(blk + 4u) == 2){
        uint32_t size = MR32(blk + 8u);
        if (!verify){
            if (jdlz_native(blk, out)){ g_jdlz_n++; g_jdlz_bytes += size; c->eax = size; POP32(c->eip); return; }
            g_jdlz_lifted++;
        } else if (range_committed(out, size)){
            uint8_t *mine = malloc(size ? size : 1);
            int done = mine && jdlz_native(blk, out);
            if (done) memcpy(mine, &MR8(out), size);
            sub_00574670_orig(c);
            if (done && memcmp(mine, &MR8(out), size)){
                uint32_t at = 0; while (at < size && mine[at] == MR8(out + at)) at++;
                fprintf(stderr, "[jdlz] f=%ld DIFFERS from the game's: block 0x%08x, %u bytes, first at %u\n", autotest_frame(), blk, size, at);
            }
            if (done) g_jdlz_n++;
            free(mine);
            return;
        }
    }
    sub_00574670_orig(c);
}

/* --- sub_0061b980: NewGetCarPart(type, slot, key, previous, level), thiscall
 * CarPartDB, ret 0x14 -------------------------------------------------------
 * A part of a car type in a ride slot (with a key, after a part, of an
 * upgrade level), found by walking the database's parts of the type's owners
 * one by one, three attribute names hashed at each (0x61B670).  A ride's
 * stock parts (0x637040) ask it for each of 170 slots, going through a slot's
 * parts one after another until one of the level, and run
 * UpdatePartsEnabled (0x61BCD0, which asks it again) after each slot: about
 * 3,900 asks a car, so every friend's car arriving in free roam spent ~1 ms
 * of the game thread here among a crowd coming and going
 * (tests/scripts/roam_crowd_busy.txt: 4.7 million asks for 1,200 cars; the
 * player's own car and the shops ask it too).  It reads the database and its
 * owner and attribute tables, which do not change once the game has loaded
 * them, and writes nothing, so its answers (no part among them: most slots
 * have none) are kept by their arguments, about 120,000 for the 31 cars:
 * all forgotten if one of those tables moves, or when PARTMEMO_N is 3/4
 * full.  NFSU2_PARTMEMO=0 (test aid): asked every time; 2: asked every time,
 * each kept answer checked against it ("[parts] DIFFERS"). */
#define PARTMEMO_N      262144                  /* a power of two: 4 MB */
#define G_CARPARTDB_OBJ 0x008a2b68u
void sub_0061b980_orig(CPU *c);
static struct { uint32_t key, prev, part; uint8_t type, slot, lvl, used; } g_pm[PARTMEMO_N];
static uint32_t g_pm_n, g_pm_tabs[4];
void sub_0061b980(CPU *restrict c){
    static int mode = -1;
    if (mode < 0){ const char *e = getenv("NFSU2_PARTMEMO"); mode = e && *e == '0' ? 0 : e && *e == '2' ? 2 : 1; }
    uint32_t e = c->esp, db = c->ecx;
    if (!mode || db != G_CARPARTDB_OBJ || !mem_is_committed(e + 0x14u)){ sub_0061b980_orig(c); return; }
    uint32_t type = MR32(e + 4u), slot = MR32(e + 8u), key = MR32(e + 12u), prev = MR32(e + 16u), lvl = MR32(e + 20u);
    if (type > 0xffu || slot > 0xffu || (int32_t)lvl < -128 || (int32_t)lvl > 127){ sub_0061b980_orig(c); return; }
    uint32_t tabs[4] = { MR32(db), MR32(0x008a1cb8u), MR32(0x008a1cbcu), MR32(0x008a1cc8u) };   /* parts, owners, their count, attributes */
    if (memcmp(tabs, g_pm_tabs, sizeof tabs) || g_pm_n >= PARTMEMO_N / 4u * 3u){
        memset(g_pm, 0, sizeof g_pm); g_pm_n = 0; memcpy(g_pm_tabs, tabs, sizeof tabs); g_pm_clears++;
    }
    g_pm_calls++;
    uint32_t h = (type * 0x85ebca77u) ^ (slot * 0xc2b2ae3du) ^ (key * 0x27d4eb2fu) ^ (prev * 0x165667b1u) ^ (lvl * 0xd3a2646cu);
    h = (h ^ (h >> 15)) * 0x2c1b3c6du;
    h = (h ^ (h >> 12)) & (PARTMEMO_N - 1u);
    for (; g_pm[h].used; h = (h + 1u) & (PARTMEMO_N - 1u)){
        if (g_pm[h].type != type || g_pm[h].slot != slot || g_pm[h].key != key || g_pm[h].prev != prev || g_pm[h].lvl != (uint8_t)lvl) continue;
        if (mode == 1){ g_pm_hits++; c->eax = g_pm[h].part; POP32(c->eip); c->esp += 0x14u; return; }
        uint32_t kept = g_pm[h].part;
        sub_0061b980_orig(c);
        if (c->eax != kept)
            fprintf(stderr, "[parts] f=%ld DIFFERS from the game's: type %u slot 0x%x key 0x%x after 0x%x level %d: 0x%x, kept 0x%x\n",
                    autotest_frame(), type, slot, key, prev, (int32_t)lvl, c->eax, kept);
        return;
    }
    sub_0061b980_orig(c);
    g_pm[h].type = (uint8_t)type; g_pm[h].slot = (uint8_t)slot; g_pm[h].key = key; g_pm[h].prev = prev; g_pm[h].lvl = (uint8_t)lvl;
    g_pm[h].part = c->eax; g_pm[h].used = 1; g_pm_n++;
}

/* --- a car's shadow outline: sub_0060cee0(points, n, comparator), cdecl ----
 * 0x60D000 projects a car's shadow mesh onto the ground and keeps the convex
 * hull, in two halves per car and frame: here the CRT qsort (0x75CC70) of the
 * points (pointers to x, y floats) by 0x60CE60 (x ascending, then y
 * descending) or 0x60CEC0 (the reverse), then a monotone chain that moves the
 * points it keeps to the front -- popping while the cross product (at PC=24)
 * is <= 0 -- and returns the index of the last.  Among 32 friends' cars it
 * was ~0.8 ms a frame, mostly the comparator reached through the dispatcher
 * (~600 calls per car).  Here: the CRT's qsort as VS2005 has it (median of
 * three, the loguy/higuy partition with the equal-element skips, the
 * selection sort below 9 elements, the smaller part first on its stack), its
 * comparisons and swaps in the same order, so equal points end up in the
 * same order; the comparators and the chain through X87R2, as the lifted
 * code computes them.  The same memory, eax,
 * ebx, ebp, esi, edi, esp, eip and x87 top as the lifted body; not ecx, edx,
 * the flags, the x87 condition bits and empty slots (0x60D000, the only
 * caller, writes those before reading them) nor the stores below the
 * caller's esp.  Another comparator runs the lifted body.  tests/main_all.c
 * section 9 checks it against the lifted body. */
void sub_0060cee0_orig(CPU *c);
uint64_t g_hull_lifted;            /* calls that ran the lifted body (tests) */
static int hull_cmp(CPU *restrict c, uint32_t ea, uint32_t eb, int rev){
    if (rev){ uint32_t t = ea; ea = eb; eb = t; }
    uint32_t A = MR32(ea), B = MR32(eb);
    double K = MRF32(0x007d7d3cu), ax = MRF32(A), bx = MRF32(B), ay = MRF32(A + 4u), by = MRF32(B + 4u);
    double d = X87R2(ax, bx, ax - bx);
    if (d > K) return 1;
    if (d < K) return -1;
    double e = X87R2(by, ay, by - ay);
    return e > K ? 1 : e < K ? -1 : 0;
}
/* The sort works on positions in the array.  With the comparators' zero
 * ([0x7D7D3C]) at 0 the points' coordinates are read once into x/y (moved
 * along with the pointers): the sign of the rounded difference is that of
 * the plain comparison of the two floats (the difference of two floats is
 * zero only when they are equal, and rounding keeps its sign), a NaN falling
 * through as before. */
typedef struct { CPU *c; uint32_t arr; float *x, *y; int rev; } HullSort;
static inline int hs_cmp(const HullSort *h, uint32_t i, uint32_t j){
    if (h->rev){ uint32_t t = i; i = j; j = t; }
    if (!h->x) return hull_cmp(h->c, h->arr + 4u * i, h->arr + 4u * j, 0);
    float ax = h->x[i], bx = h->x[j];
    if (ax > bx) return 1;
    if (ax < bx) return -1;
    float ay = h->y[i], by = h->y[j];
    return by > ay ? 1 : by < ay ? -1 : 0;
}
static inline void hs_swap(const HullSort *h, uint32_t i, uint32_t j){
    uint32_t a = h->arr + 4u * i, b = h->arr + 4u * j, t = MR32(a); MR32(a) = MR32(b); MR32(b) = t;
    if (h->x){ float u = h->x[i]; h->x[i] = h->x[j]; h->x[j] = u; u = h->y[i]; h->y[i] = h->y[j]; h->y[j] = u; }
}
/* positions are signed: the partition can step one below the array, as the
 * CRT's pointer does */
static void hull_qsort(const HullSort *h, int32_t num){
    int32_t lostk[32], histk[32], lo = 0, hi = num - 1;
    int sp = 0;
    if (num < 2) return;
    for (;;){
        int32_t size = hi - lo + 1;
        if (size <= 8){
            while (hi > lo){
                int32_t mx = lo;
                for (int32_t p = lo + 1; p <= hi; p++) if (hs_cmp(h, (uint32_t)p, (uint32_t)mx) > 0) mx = p;
                hs_swap(h, (uint32_t)mx, (uint32_t)hi); hi--;
            }
        } else {
            int32_t mid = lo + size / 2, loguy = lo, higuy = hi;
            if (hs_cmp(h, lo, mid) > 0) hs_swap(h, lo, mid);
            if (hs_cmp(h, lo, hi) > 0) hs_swap(h, lo, hi);
            if (hs_cmp(h, mid, hi) > 0) hs_swap(h, mid, hi);
            for (;;){
                if (mid > loguy) do loguy++; while (loguy < mid && hs_cmp(h, loguy, mid) <= 0);
                if (mid <= loguy) do loguy++; while (loguy <= hi && hs_cmp(h, loguy, mid) <= 0);
                do higuy--; while (higuy > mid && hs_cmp(h, higuy, mid) > 0);
                if (higuy < loguy) break;
                hs_swap(h, loguy, higuy);
                if (mid == higuy) mid = loguy;
            }
            higuy++;
            if (mid < higuy) do higuy--; while (higuy > mid && hs_cmp(h, higuy, mid) == 0);
            if (mid >= higuy) do higuy--; while (higuy > lo && hs_cmp(h, higuy, mid) == 0);
            if (higuy - lo >= hi - loguy){
                if (lo < higuy){ lostk[sp] = lo; histk[sp] = higuy; sp++; }
                if (loguy < hi){ lo = loguy; continue; }
            } else {
                if (loguy < hi){ lostk[sp] = loguy; histk[sp] = hi; sp++; }
                if (lo < higuy){ hi = higuy; continue; }
            }
        }
        if (--sp < 0) return;
        lo = lostk[sp]; hi = histk[sp];
    }
}
void sub_0060cee0(CPU *restrict c){
#ifdef __wasm__
    uint32_t e = c->esp, arr = MR32(e + 4u), cmp = MR32(e + 12u);
    int32_t n = (int32_t)MR32(e + 8u);
    if (cmp == 0x0060ce60u || cmp == 0x0060cec0u){
        static float hx[1024], hy[1024];
        HullSort h = { c, arr, NULL, NULL, cmp == 0x0060cec0u };
        if (MRF32(0x007d7d3cu) == 0.0f && n <= 1024){
            h.x = hx; h.y = hy;
            for (int32_t i = 0; i < n; i++){ uint32_t p = MR32(arr + 4u * (uint32_t)i); hx[i] = MRF32(p); hy[i] = MRF32(p + 4u); }
        }
        hull_qsort(&h, n);
        uint32_t last = 1;
        for (int32_t i = 2; i < n; i++){
            uint32_t k = last;
            if ((int32_t)k >= 1){
                uint32_t p = MR32(arr + 4u * (uint32_t)i);
                double px = MRF32(p), py = MRF32(p + 4u), K = MRF32(0x007d7d3cu);
                for (;;){
                    uint32_t a = MR32(arr + 4u * k), q = MR32(arr + 4u * k - 4u);
                    double ax = MRF32(a), ay = MRF32(a + 4u), qx = MRF32(q), qy = MRF32(q + 4u);
                    double u = X87R2(qy, ay, qy - ay), v = X87R2(px, ax, px - ax), t1 = X87R2(u, v, u * v);
                    double w = X87R2(py, ay, py - ay), s = X87R2(qx, ax, qx - ax), t2 = X87R2(w, s, w * s);
                    double t = X87R2(t1, t2, t1 - t2);
                    if (!(t <= K)) break;                      /* > 0 or unordered: keep a */
                    if ((int32_t)--k < 1) break;
                }
            }
            uint32_t a = MR32(arr + 4u * (k + 1u));
            last = k + 1u;
            MR32(arr + 4u * last) = MR32(arr + 4u * (uint32_t)i);
            MR32(arr + 4u * (uint32_t)i) = a;
        }
        c->eax = last;
        POP32(c->eip);
        return;
    }
#endif
    g_hull_lifted++;
    sub_0060cee0_orig(c);
}

/* --- spherical-harmonic lighting: sub_004895e0, thiscall, ret 0x14 ---------
 * (this = 10 x 4 floats of SH coefficients, colour[4], scale, x, y, z): adds
 * one light from direction (x, y, z) with colour * scale to the nine
 * coefficients (and a tenth row), one colour channel at a time.  0x489AF0
 * calls it twice per light, 0x48ED60 for up to 18 lights per car and frame
 * (~0.6 ms a frame among 32 friends' cars).  The four channels do the same
 * x87 operations, so here they are the lanes of f32x4 operations, in the
 * same order with the same operands: at PC=24 and RC=nearest the x87 rounds
 * each result of float operands to 24 bits as f32 arithmetic does (the
 * lifted X87R2 gives the same) while it is a normal float, or zero from a
 * zero operand or an exact cancellation.  Every result is checked for that
 * (a product of non-zero operands that comes out zero underflowed); anything
 * else (NaN, infinity, subnormal, underflow, another control word, the
 * coefficients overlapping the colour) runs the lifted body from the
 * untouched state.  The same coefficients, eax, ecx, edx, flags, esp, eip,
 * x87 top and condition bits as the lifted body, and its store of
 * 3 z^2 K - K' over the colour argument; not the scratch stores below the
 * caller's esp nor the x87 slots it leaves empty.  tests/main_all.c section
 * 10 checks it against the lifted body.  The Mac keeps the lifted body. */
void sub_004895e0_orig(CPU *c);
uint64_t g_sh_lifted;              /* calls that ran the lifted body (tests) */
#ifdef __wasm__
#pragma clang fp contract(off)
typedef float sh4 __attribute__((vector_size(16), aligned(4)));
typedef int32_t shi4 __attribute__((vector_size(16), aligned(4)));
/* not a normal float and not zero: NaN, infinity, subnormal */
static inline shi4 sh_bad(sh4 r){
    sh4 a = (sh4)((shi4)r & 0x7fffffff);
    return ((a < 0x1p-126f) & (r != 0.0f)) | (a > 0x1.fffffep127f) | (r != r);
}
#define SH_MUL(r, a, b) do{ sh4 _a = (a), _b = (b); r = _a * _b; bad |= sh_bad(r) | ((r == 0.0f) & (_a != 0.0f) & (_b != 0.0f)); }while(0)
#define SH_ADD(r, a, b) do{ r = (a) + (b); bad |= sh_bad(r); }while(0)
#define SH_SUB(r, a, b) do{ r = (a) - (b); bad |= sh_bad(r); }while(0)
static int sh_native(CPU *restrict c){
    if (c->fcw & 0x0F00u) return 0;
    uint32_t e = c->esp, o = c->ecx, col = MR32(e + 4u);
    /* the coefficients [o, o+0xA0) against the colour [col, col+16) and the return address and arguments [e, e+0x18) */
    if (col - o + 15u < 15u + 0xa0u || e - o + 0x17u < 0x17u + 0xa0u) return 0;
    sh4 cv, O[10], C, A, B, D, xD, S18, S1c, S20, S24, S28, S30, S2c, S34, E, G, t, u;
    shi4 bad = { 0, 0, 0, 0 };
    memcpy(&cv, g_mem + col, 16);
    for (int i = 0; i < 10; i++) memcpy(&O[i], g_mem + o + 16u * (uint32_t)i, 16);
    float s = MRF32(e + 8u), x = MRF32(e + 12u), y = MRF32(e + 16u), z = MRF32(e + 20u);
    sh4 X = { x, x, x, x }, Y = { y, y, y, y }, Z = { z, z, z, z }, S = { s, s, s, s };
#define SHK(a) ((sh4){ MRF32(a), MRF32(a), MRF32(a), MRF32(a) })
    sh4 T3, T4;                                           /* lanes equal */
    SH_MUL(t, Z, Z); SH_MUL(t, t, SHK(0x787adcu)); SH_SUB(T3, t, SHK(0x784250u));
    SH_MUL(t, X, X); SH_MUL(u, Y, Y); SH_SUB(T4, t, u);
    SH_MUL(C, cv, S);
    SH_MUL(A, SHK(0x78d944u), C);
    SH_MUL(B, C, SHK(0x78d940u));
    SH_MUL(S18, Y, B); SH_MUL(S1c, Z, B); SH_MUL(S20, B, X);
    SH_MUL(D, C, SHK(0x78d93cu));
    SH_MUL(xD, X, D); SH_MUL(S24, Y, xD);
    SH_MUL(t, D, Y); SH_MUL(S28, t, Z);
    SH_MUL(S30, xD, Z);
    SH_MUL(t, C, SHK(0x78d938u)); SH_MUL(S2c, t, T3);
    SH_MUL(t, C, SHK(0x78d934u)); SH_MUL(S34, t, T4);
    SH_MUL(A, A, SHK(0x78d930u));
    SH_MUL(t, S2c, SHK(0x78d92cu)); SH_SUB(E, A, t);
    SH_ADD(O[0], E, O[0]);
    SH_MUL(G, S34, SHK(0x78d928u));
    SH_ADD(O[1], G, O[1]);
    SH_SUB(O[2], O[2], G);
    SH_MUL(t, S2c, SHK(0x78d924u)); SH_ADD(O[3], t, O[3]);
    SH_MUL(t, S24, SHK(0x78d920u)); SH_ADD(O[4], t, O[4]);
    SH_MUL(t, S30, SHK(0x78d920u)); SH_ADD(O[5], t, O[5]);
    SH_MUL(t, S28, SHK(0x78d928u)); SH_ADD(O[6], t, O[6]);
    SH_MUL(t, S20, SHK(0x78d91cu)); SH_ADD(O[7], t, O[7]);
    SH_MUL(t, S18, SHK(0x78d91cu)); SH_ADD(O[8], t, O[8]);
    SH_MUL(t, S1c, SHK(0x78d91cu)); SH_ADD(O[9], t, O[9]);
#undef SHK
    if (bad[0] | bad[1] | bad[2] | bad[3]) return 0;
    for (int i = 0; i < 10; i++) memcpy(g_mem + o + 16u * (uint32_t)i, &O[i], 16);
    c->eax = MR32(col + 12u); c->edx = MR32(col + 8u);
    MRF32(e + 4u) = T3[0];                                /* its fstp [esp+0x3c] */
    { uint32_t a = e - 0x38u, r = e; FLAGS_ADD_M(a, 0x38u, r, 32, F_ALL); }    /* add esp, 0x38 */
    POP32(c->eip); c->esp += 0x14u;
    return 1;
}
/* sub_00489ba0(M), thiscall, ret 4: rotates the coefficients at this by the
 * 3x3 matrix in rows 0-2 of M (0x48E6C0, per part group of a car), each
 * channel by the same x87 sequence: here as f32x4 lanes like sh_native, the
 * operations in its order.  Old coefficients in, new ones out (C0 is written
 * back as it was).  The same coefficients, eax (this + 0x30), ecx (0), edx
 * (the last channel's C0), flags, esp, eip, x87 top and condition bits as
 * the lifted body, and its store of M[4] over the argument.  tests/main_all.c
 * section 11 checks it. */
static int shrot_native(CPU *restrict c){
    if (c->fcw & 0x0F00u) return 0;
    uint32_t e = c->esp, o = c->ecx, M = MR32(e + 4u);
    if (M - o + 0x2bu < 0x2bu + 0xa0u || e - o + 7u < 7u + 0xa0u) return 0;   /* overlap */
    sh4 C[10], N[10], t, u, w, T44, T48, T4c, T50, T54, T58, T2c, T30, T34;
    shi4 bad = { 0, 0, 0, 0 };
    for (int j = 0; j < 10; j++) memcpy(&C[j], g_mem + o + 16u * (uint32_t)j, 16);
#define SHM(off) ((sh4){ MRF32(M + (off)), MRF32(M + (off)), MRF32(M + (off)), MRF32(M + (off)) })
    sh4 a = SHM(0x00u), b = SHM(0x10u), cc = SHM(0x20u), d = SHM(0x04u), ee = SHM(0x14u), f = SHM(0x24u),
        g = SHM(0x08u), h = SHM(0x18u), ii = SHM(0x28u);
#undef SHM
    /* C1 */
    SH_MUL(t, C[6], b); SH_MUL(u, C[3], cc); SH_ADD(t, t, u); SH_MUL(u, C[5], a); SH_ADD(t, t, u); SH_MUL(t, t, cc);
    SH_MUL(u, C[2], b); SH_MUL(w, C[4], a); SH_ADD(u, u, w); SH_MUL(u, u, b); SH_ADD(t, t, u);
    SH_MUL(u, C[1], a); SH_MUL(u, u, a); SH_ADD(N[1], t, u);
    /* C2 */
    SH_MUL(T58, C[6], ee); SH_MUL(T54, f, C[3]); SH_MUL(T50, C[5], d); SH_MUL(T4c, C[2], ee);
    SH_MUL(T48, C[4], d); SH_MUL(T44, C[1], d);
    SH_ADD(t, T50, T54); SH_ADD(t, t, T58); SH_MUL(t, t, f);
    SH_ADD(u, T48, T4c); SH_MUL(u, u, ee); SH_ADD(t, t, u);
    SH_MUL(u, T44, d); SH_ADD(N[2], t, u);
    /* C3 */
    sh4 C2h, C4g, C1g, S;
    SH_MUL(T34, C[6], h); SH_MUL(T30, C[3], ii); SH_MUL(T2c, C[5], g);
    SH_MUL(C2h, C[2], h); SH_MUL(C4g, C[4], g); SH_MUL(C1g, C[1], g);
    SH_ADD(S, T2c, T30); SH_ADD(S, S, T34); SH_MUL(t, S, ii);
    SH_ADD(u, C4g, C2h); SH_MUL(u, u, h); SH_ADD(t, t, u);
    SH_MUL(u, C1g, g); SH_ADD(N[3], t, u);
    /* C4 */
    SH_MUL(t, C[6], f); SH_MUL(t, t, b); SH_MUL(u, C[5], f); SH_MUL(u, u, a); SH_ADD(t, t, u);
    SH_MUL(u, C[4], ee); SH_MUL(u, u, a); SH_ADD(t, t, u);
    SH_MUL(u, T44, a); SH_ADD(u, u, u); SH_ADD(t, t, u);
    SH_MUL(u, T48, b); SH_ADD(t, t, u);
    SH_MUL(u, T4c, b); SH_ADD(u, u, u); SH_ADD(t, t, u);
    SH_MUL(u, T50, cc); SH_ADD(t, t, u);
    SH_MUL(u, T54, cc); SH_ADD(u, u, u); SH_ADD(t, t, u);
    SH_MUL(u, T58, cc); SH_ADD(N[4], t, u);
    /* C5 */
    sh4 C6i, C5i, C4h, K1;
    SH_MUL(C6i, C[6], ii); SH_MUL(C5i, C[5], ii); SH_MUL(C4h, C[4], h);
    SH_ADD(K1, C4h, C5i);
    SH_MUL(t, K1, a); SH_MUL(u, C6i, b); SH_ADD(t, t, u);
    SH_MUL(u, C1g, a); SH_ADD(u, u, u); SH_ADD(t, t, u);
    SH_MUL(u, C4g, b); SH_ADD(t, t, u);
    SH_MUL(u, C2h, b); SH_ADD(u, u, u); SH_ADD(t, t, u);
    SH_MUL(u, T2c, cc); SH_ADD(t, t, u);
    SH_MUL(u, T30, cc); SH_ADD(u, u, u); SH_ADD(t, t, u);
    SH_MUL(u, T34, cc); SH_ADD(N[5], t, u);
    /* C6 */
    SH_MUL(t, K1, d); SH_MUL(u, C6i, ee); SH_ADD(t, t, u);
    SH_MUL(u, C1g, d); SH_ADD(u, u, u); SH_ADD(t, t, u);
    SH_MUL(u, C4g, ee); SH_ADD(t, t, u);
    SH_MUL(u, C2h, ee); SH_ADD(u, u, u); SH_ADD(t, t, u);
    SH_MUL(u, T2c, f); SH_ADD(t, t, u);
    SH_MUL(u, T30, f); SH_ADD(u, u, u); SH_ADD(t, t, u);
    SH_MUL(u, T34, f); SH_ADD(N[6], t, u);
    /* C7, C8, C9 */
    SH_MUL(t, C[9], cc); SH_MUL(u, C[8], b); SH_ADD(t, t, u); SH_MUL(u, C[7], a); SH_ADD(N[7], t, u);
    SH_MUL(t, C[9], f); SH_MUL(u, C[8], ee); SH_ADD(t, t, u); SH_MUL(u, C[7], d); SH_ADD(N[8], t, u);
    SH_MUL(t, C[9], ii); SH_MUL(u, C[8], h); SH_ADD(t, t, u); SH_MUL(u, C[7], g); SH_ADD(N[9], t, u);
    if (bad[0] | bad[1] | bad[2] | bad[3]) return 0;
    for (int j = 1; j < 10; j++) memcpy(g_mem + o + 16u * (uint32_t)j, &N[j], 16);
    c->eax = o + 0x30u; c->ecx = 0; c->edx = MR32(o + 12u);
    MR32(e + 4u) = MR32(M + 0x10u);                       /* its mov [esp+0x60], edx */
    { uint32_t x = e - 0x5cu, r = e; FLAGS_ADD_M(x, 0x5cu, r, 32, F_ALL); }    /* add esp, 0x5c */
    POP32(c->eip); c->esp += 4u;
    return 1;
}
#undef SH_MUL
#undef SH_ADD
#undef SH_SUB
#pragma clang fp contract(on)
#endif
void sub_004895e0(CPU *restrict c){
#ifdef __wasm__
    if (sh_native(c)) return;
#endif
    g_sh_lifted++;
    sub_004895e0_orig(c);
}
void sub_00489ba0_orig(CPU *c);
uint64_t g_shrot_lifted;           /* calls that ran the lifted body (tests) */
void sub_00489ba0(CPU *restrict c){
#ifdef __wasm__
    if (shrot_native(c)) return;
#endif
    g_shrot_lifted++;
    sub_00489ba0_orig(c);
}
