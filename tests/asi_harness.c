/* asi_harness.c -- does the Windows plugin do what the Mac port does?
 *
 * Runs the real NFSU2FreeRoam.asi (32-bit x86, relocated to a base it was not
 * linked for) under Unicorn on one copy of a guest-memory snapshot taken in
 * career free roam on the port (tests/scripts/roam_asi_snapshot.txt), and
 * the Mac build of the very same runtime/roam_game.c (arm64, the port's host
 * layer: guest memory at g_mem, guest calls through dispatch_indirect) on a
 * second copy.  Both get exactly the same inputs: a stand-in friend (the
 * real core, in this process) that joins, sends its car look and drives a
 * circle next to the player; the same clock (8.333 ms per frame); the same
 * world dt.  The game functions both of them call run as the game's original
 * x86 code under Unicorn, each on its own copy; the few that would load
 * files or touch Direct3D are replaced, identically for both, by stubs.
 *
 * Checked every frame:
 *   - the datagrams the plugin sends (its Winsock sendto) and the ones the Mac
 *     build sends: byte for byte (so also the wire format of the 32-bit core),
 *     except a HELLO's platform byte (18: 2 from the plugin, 1 from the Mac)
 *     and with it the CRC;
 *   - the game functions called, in order, with ecx, edx and the arguments;
 *   - every byte of guest memory outside the stack and the plugin's own image
 *     (the friend's car: pose, controls, ride, racer record; the encounter
 *     manager; the game heap).
 * And for the plugin alone: DllMain + InitializeASI change exactly the 5
 * bytes of the frame hook; the frame thunk returns to the game with every
 * register and the flags as they were.
 *
 *   out/asi_harness [--frames N] [--fault|--faultself|--badexe|--chain] <snap.bin> <cfg.bin> <outdir>
 *   The stand-in friend leaves (BYE) half-way and comes back (a new id, the
 *   same car) at 70 %: the car is parked, then shown again without a load.
 *     --fault   in frame 300, while the friend's car is shown, a game function
 *               the plugin calls faults (NFSU2_ROAM_TEST_FAULT=300 gives it a
 *               bad object): the plugin's vectored exception handler must
 *               bring the game back, the plugin must switch itself off (BYE
 *               to the friend) and do nothing more, frame after frame
 *     --faultself  the same with a bad write in the plugin's own code
 *     --faultseh   the same inside a game function that has put an SEH frame
 *               into FS:[0] (NFSU2_ROAM_TEST_FAULT_SEH=300: SetDriverType with
 *               a bad object): FS:[0] must be back as it was at the hook
 *               (checked after every frame in every mode)
 *     --badexe  one byte of a game function the plugin calls is changed
 *               before it loads: it must not hook anything
 *     --chain   another mod's call is at 0x581475 before the plugin loads:
 *               the plugin must call it after its own frame step
 *     --unlimiter  UpdatePartsEnabled (0x61BCD0) starts with a jump to code
 *               elsewhere, as Unlimiter replaces it: the exe check must
 *               accept it and the plugin's calls must go through it
 * Exit 0 when every check passes.
 */
#include "cpu.h"
#include "roam_core.h"
#include "roam_plat.h"
#include "carsfx.h"
#include "asi_vm.h"
#include "asi_win32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <unistd.h>
#include <crt_externs.h>
#include <math.h>

void roam_game_frame(CPU *c);
void roam_game_report(void);

#define HOOK_SITE   0x00581475u
#define HOOK_NEXT   0x0058147Au
#define TEXT_LO     0x00401000u
#define TEXT_HI     0x00783000u
#define ASI_BASE    0x01000000u          /* not the plugin's preferred 0x10000000: relocations are exercised */
#define ENGINE_PORT 47210
#define FAKE_PORT   47391
#define TEST_ID     0x1D0E5E1Du
#define FRAME_US    8333
#define G_WORLDDT   0x00865154u
#define G_WORLD     0x00890080u
#define G_PLAYER1   0x008900acu

/* ---- the two arenas --------------------------------------------------------------- */
uint8_t *g_mem;                           /* B: the Mac build (roam_game.c through cpu.h) */
uint8_t  g_cbits[1u << 15];
static uint8_t *g_memA;                   /* A: the plugin */
static uint8_t  g_cbitsA[1u << 15];
static uint32_t g_snap_hdr[13];
static uint32_t g_hook_esp;
static int committed_bits(const uint8_t *bits, uint32_t a){ uint32_t b = a >> 14; return (bits[b >> 3] >> (b & 7)) & 1; }
static int committedA(void *u, uint32_t a){ (void)u; return committed_bits(g_cbitsA, a); }
static int committedB(void *u, uint32_t a){ (void)u; return committed_bits(g_cbits, a); }
static uint8_t *arena(void){
    uint8_t *m = mmap(0, 1ull << 32, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED){ perror("mmap"); exit(2); }
    return m;
}
static void commit(uint8_t *m, uint8_t *bits, uint32_t a, uint32_t n){
    for (uint32_t b = a >> 14; b <= (a + n - 1) >> 14; b++){
        if (!((bits[b >> 3] >> (b & 7)) & 1)){
            mprotect(m + ((uint64_t)b << 14), 0x4000, PROT_READ | PROT_WRITE);
            bits[b >> 3] |= (uint8_t)(1u << (b & 7));
        }
    }
}
static void commitA(void *u, uint32_t a, uint32_t n){ (void)u; commit(g_memA, g_cbitsA, a, n); }
static uint32_t g_nblocks;
static int snap_load(const char *path){
    FILE *f = fopen(path, "rb");
    if (!f){ perror(path); return -1; }
    char mg[8];
    if (fread(mg, 1, 8, f) != 8 || memcmp(mg, "NFSUSNP1", 8) || fread(g_snap_hdr, 4, 13, f) != 13){ fprintf(stderr, "%s: not a snapshot\n", path); return -1; }
    g_nblocks = g_snap_hdr[12];
    for (uint32_t i = 0; i < g_nblocks; i++){
        uint32_t b;
        if (fread(&b, 4, 1, f) != 1 || b >= (1u << 18)) return -1;
        commit(g_mem, g_cbits, b << 14, 0x4000); commit(g_memA, g_cbitsA, b << 14, 0x4000);
        if (fread(g_mem + ((uint64_t)b << 14), 1, 0x4000, f) != 0x4000) return -1;
        memcpy(g_memA + ((uint64_t)b << 14), g_mem + ((uint64_t)b << 14), 0x4000);
    }
    fclose(f);
    g_hook_esp = g_snap_hdr[6];
    return 0;
}

/* ---- results ----------------------------------------------------------------------- */
static int g_fail; static FILE *g_out;
static void say(const char *fmt, ...){ va_list ap; va_start(ap, fmt); vfprintf(g_out, fmt, ap); va_end(ap); fflush(g_out); }
static void check(int ok, const char *fmt, ...){
    va_list ap; va_start(ap, fmt);
    fprintf(g_out, ok ? "  ok    " : "  FAIL  "); vfprintf(g_out, fmt, ap); fprintf(g_out, "\n"); fflush(g_out);
    va_end(ap);
    if (!ok) g_fail++;
}

/* ---- the clock, the frame number ---------------------------------------------------- */
static int64_t g_now = 1000000000;       /* host µs, both engines */
static long g_frame;
long autotest_frame(void){ return g_frame; }
uint32_t win32_heap_live(void){ return 0; }
int64_t roam_plat_now_us(void){ return g_now; }
int horn_held(int stick){ (void)stick; return 0; }   /* the companion has no horn */
int horn_mine(void){ return -1; }
const char *horn_name(int kind){ (void)kind; return "dual"; }
void horn_begin(const float *pos, const float *fwd, const float *vel){ (void)pos; (void)fwd; (void)vel; }
int horn_sound(uint32_t who, const char *name, int kind, const float *p, const float *v){ (void)who; (void)name; (void)kind; (void)p; (void)v; return 0; }
void horn_end(void){}
void carsfx_begin(const float *pos, const float *fwd, const float *vel){ (void)pos; (void)fwd; (void)vel; }
void carsfx_car(uint32_t who, const char *name, const float *p, const float *v, const carsfx_state *st){ (void)who; (void)name; (void)p; (void)v; (void)st; }
void carsfx_end(void){}

/* ---- datagrams ---------------------------------------------------------------------- */
typedef struct { uint32_t ip; uint16_t port; int len; uint8_t b[RC_MAX_DGRAM]; } Dg;
typedef struct { Dg q[256]; int n, h; } Box;
static void box_put(Box *x, uint32_t ip, uint16_t port, const uint8_t *b, int len){
    if (x->n == 256 || len > RC_MAX_DGRAM) return;
    Dg *d = &x->q[(x->h + x->n++) % 256]; d->ip = ip; d->port = port; d->len = len; memcpy(d->b, b, (size_t)len);
}
static int box_get(Box *x, Dg *o){ if (!x->n) return 0; *o = x->q[x->h]; x->h = (x->h + 1) % 256; x->n--; return 1; }

/* each engine: its inbox (from its stand-in friend), what it sent this
 * frame, its friend, its call trace, its stubs' state */
typedef struct { uint32_t fn, ecx, edx, a[5]; int n; } Call;
typedef struct Eng {
    const char *name;
    Box in;
    Dg  out[64]; int nout, nout_total;
    rc_session fk;
    Call tr[4096]; int ntr;
    int byes, bye_frame, sent_after_bye, gone;
    uint32_t loads[32]; int nloads, polls[32];
    uint32_t setride_last; int setrides;
    xvm *vm;
} Eng;
static Eng EA = { .name = "plugin" }, EB = { .name = "mac" };
static void out_put(Eng *e, uint32_t ip, uint16_t port, const uint8_t *b, int len){
    if (len > 5 && b[5] == RC_MSG_BYE){ if (!e->byes++) e->bye_frame = (int)g_frame; }
    else if (e->byes) e->sent_after_bye++;
    if (e->nout < 64){ Dg *d = &e->out[e->nout++]; d->ip = ip; d->port = port; d->len = len; memcpy(d->b, b, (size_t)len); }
    e->nout_total++;
}

/* ---- the stand-in friend: the real core, a car on a circle next to the player -------- */
static uint8_t g_cfg[RC_CFG_MAX]; static int g_cfg_len;
static float g_ctr[3];
static void fk_send(void *u, uint32_t ip, uint16_t port, const uint8_t *buf, int len){
    Eng *e = u; (void)ip; (void)port;
    box_put(&e->in, 0x7F000001u, FAKE_PORT, buf, len);
}
static void fk_bcast(void *u, uint16_t port, const uint8_t *b, int n){ (void)u; (void)port; (void)b; (void)n; }
static void fk_event(void *u, int ev, int peer, const char *t){ (void)u; (void)ev; (void)peer; (void)t; }
static void fake_init(Eng *e, uint32_t id){
    rc_config c; memset(&c, 0, sizeof c);
    e->gone = 0;
    c.self_id = id; c.port = FAKE_PORT; c.room = 0; snprintf(c.name, sizeof c.name, "Friend");
    c.platform = RC_PLAT_TOOL; c.test_mode = 1; c.max_players = 8; c.rate_hz = 20;
    rc_io io = { e, fk_send, fk_bcast, fk_event };
    rc_session_init(&e->fk, &c, &io);
    rc_add_peer(&e->fk, 0x7F000001u, ENGINE_PORT, 1, g_now);
    rc_set_status(&e->fk, RC_ST_ROAM);
    rc_set_cfg(&e->fk, g_cfg, g_cfg_len, g_now);
}
static float fsin(float x){ /* small-angle series after range reduction: test data only */
    while (x > 3.14159265f) x -= 6.2831853f; while (x < -3.14159265f) x += 6.2831853f;
    float x2 = x * x; return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110)))));
}
static float fcos(float x){ return fsin(x + 1.5707963f); }
static void fake_state(rc_state *s, float *pos){
    float t = (float)((double)(g_now - 1000000000) / 1e6), w = 1.2f, R = 12.f, th = w * t;
    memset(s, 0, sizeof *s);
    s->t_us = (uint32_t)g_now;
    s->p[0] = g_ctr[0] + R * fcos(th); s->p[1] = g_ctr[1] + R * fsin(th); s->p[2] = g_ctr[2];
    s->v[0] = -R * w * fsin(th); s->v[1] = R * w * fcos(th);
    float h = th + 1.5707963f;
    s->q[2] = fsin(h / 2); s->q[3] = fcos(h / 2);
    s->w[2] = w; s->speed = R * w; s->rpm = 3200; s->gear = 3; s->throttle = 0.4f; s->steer = 0.25f;
    if (pos) memcpy(pos, s->p, 12);
}
static void fake_step(Eng *e, float *pos){
    if (e->gone) return;
    rc_state s; fake_state(&s, pos);
    rc_push_state(&e->fk, &s, g_now);
    rc_tick(&e->fk, g_now);
}
static void fake_take(Eng *e){
    if (e->gone) return;
    for (int i = 0; i < e->nout; i++) if (e->out[i].port == FAKE_PORT) rc_receive(&e->fk, 0x7F000001u, ENGINE_PORT, e->out[i].b, e->out[i].len, g_now);
}

/* ---- the game functions that load files or draw: the same stubs for both -------------- */
#define FN_LOADER_LOAD    0x0063bc70u
#define FN_LOADER_SERVICE 0x0063bcd0u
#define FN_LOADER_LOADED  0x00610910u
#define FN_LOADER_WAIT    0x0061ca50u
#define FN_LOADER_UNLOAD  0x0063ae00u
#define FN_CAR_SETRIDE    0x00601110u
static Eng *eng_of(void *u){ return u; }
typedef struct { uint32_t brk; } Heap;
static Heap g_heapA = { 0x2E000000u }, g_heapB = { 0x2E000000u };
static uint32_t heap_alloc(Eng *e, uint32_t size);
/* CarLoader::Load(ride, 0, 0): reads the car's files -> an entry of our own on
 * the loader's list (head 0x8A3288 + 0x58, next at +0, prev at +4, the handle
 * at +0x123C), the same in both arenas */
static int st_load(void *u, xvm *vm, uint32_t a, uint32_t *r){
    (void)a; Eng *e = eng_of(u);
    static uint32_t next_handle[2] = { 0x51000, 0x51000 };
    uint32_t h = ++next_handle[e == &EB];
    uint32_t ent = heap_alloc(e, 0x1240u), head = 0x008a3288u + 0x58u, first = xvm_rd32(vm, head);
    uint32_t v[2] = { first, head }; xvm_write(vm, ent, v, 8);
    xvm_write(vm, head, &ent, 4); if (first) xvm_write(vm, first + 4u, &ent, 4);
    xvm_write(vm, ent + 0x123Cu, &h, 4);
    r[XR_EAX] = h; return 12;
}
/* CarLoader::Service(0, 0): streaming I/O -> nothing */
static int st_service(void *u, xvm *vm, uint32_t a, uint32_t *r){ (void)u; (void)vm; (void)a; r[XR_EAX] = 0; return 8; }
/* CarLoader::IsLoaded(entry): loaded on the 5th poll */
static int st_loaded(void *u, xvm *vm, uint32_t a, uint32_t *r){
    (void)a; Eng *e = eng_of(u); uint32_t ent = xvm_rd32(vm, r[XR_ESP] + 4);
    int k = 0; while (k < e->nloads && e->loads[k] != ent) k++;
    if (k == e->nloads && k < 32){ e->loads[k] = ent; e->polls[k] = 0; e->nloads++; }
    r[XR_EAX] = k < 32 && ++e->polls[k] >= 5; return 4;
}
static int st_wait(void *u, xvm *vm, uint32_t a, uint32_t *r){ (void)u; (void)vm; (void)a; r[XR_EAX] = 1; return 4; }
static int st_unload(void *u, xvm *vm, uint32_t a, uint32_t *r){ (void)u; (void)vm; (void)a; r[XR_EAX] = 0; return 4; }
/* Car::SetRideInfo(ride): rebuilds the render info (textures) -> remember the ride */
static int st_setride(void *u, xvm *vm, uint32_t a, uint32_t *r){
    (void)a; Eng *e = eng_of(u); e->setride_last = xvm_rd32(vm, r[XR_ESP] + 4); e->setrides++; r[XR_EAX] = 0; return 4; }
/* the shared part packs (roam_game.c part_packs): loading one reads a
 * file -> nothing, and the streamer has none of them, so no ride waits */
#define FN_PACK_LOAD      0x004941c0u
#define FN_PACK_FIND      0x0048ce80u
static int st_packload(void *u, xvm *vm, uint32_t a, uint32_t *r){ (void)u; (void)vm; (void)a; r[XR_EAX] = 0; return 0; }
static int st_packfind(void *u, xvm *vm, uint32_t a, uint32_t *r){ (void)u; (void)vm; (void)a; r[XR_EAX] = 0; return 4; }
static void add_stubs(xvm *vm){
    xvm_stub(vm, FN_LOADER_LOAD, st_load);
    xvm_stub(vm, FN_LOADER_SERVICE, st_service); xvm_stub(vm, FN_LOADER_LOADED, st_loaded);
    xvm_stub(vm, FN_LOADER_WAIT, st_wait); xvm_stub(vm, FN_LOADER_UNLOAD, st_unload);
    xvm_stub(vm, FN_CAR_SETRIDE, st_setride);
    xvm_stub(vm, FN_PACK_LOAD, st_packload); xvm_stub(vm, FN_PACK_FIND, st_packfind);
}
/* the arguments each function takes (for the trace comparison) */
static int fn_nargs(uint32_t fn){
    static const uint32_t T[][2] = { {0x575620,1},{0x5f6d50,0},{0x5f6e20,0},{0x5eba30,1},{0x5f46a0,1},{0x5f6cd0,0},{0x5da330,1},
        {0x5f5010,0},{0x601110,1},{0x5930f0,0},{0x610130,1},{0x61b980,5},{0x610270,4},{0x637040,1},{0x61bcd0,0},{0x61c280,1},
        {0x5ac4b0,1},{0x5b4fc0,3},{0x63bc70,3},{0x63bcd0,2},{0x610910,1},{0x61ca50,1},{0x63ae00,1},{0x606480,2},{0x5f0d20,0},
        {0x61b670,5},{0x610000,0},{0x4941c0,4},{0x48ce80,1} };
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) if (T[i][0] == fn) return (int)T[i][1];
    return 5;
}
static void trace_add(Eng *e, xvm *vm, uint32_t fn, const uint32_t *r){
    if (fn == 0x004022C0u) return;                       /* the plugin's chain jump: not a call of ours */
    if (e->ntr == 4096) return;
    Call *c = &e->tr[e->ntr++];
    memset(c, 0, sizeof *c); c->fn = fn; c->ecx = r[XR_ECX]; c->edx = r[XR_EDX]; c->n = fn_nargs(fn);
    for (int i = 0; i < c->n && i < 5; i++) c->a[i] = xvm_rd32(vm, r[XR_ESP] + 4u + 4u * (uint32_t)i);
}
static void on_trace_A(void *u, xvm *vm, uint32_t from, uint32_t to){
    (void)from; uint32_t r[XR__N]; xvm_get(vm, r); trace_add(eng_of(u), vm, to, r); }

/* ---- the game's own imports (its IAT holds the port's HLE thunk addresses,
 * 0xF0000000 + i): the few its functions call here, the same for both ----------- */
static char g_impname[512][40];
static void game_imports(const char *exe){
    /* the exe's import names by IAT slot, then the slots' values in the snapshot */
    FILE *f = fopen(exe, "rb"); if (!f){ perror(exe); return; }
    static uint8_t d[0x600000]; size_t n = fread(d, 1, sizeof d, f); fclose(f);
    uint32_t nt = *(uint32_t *)(d + 0x3C); uint16_t nsec = *(uint16_t *)(d + nt + 6), opt = *(uint16_t *)(d + nt + 20);
    const uint8_t *sh = d + nt + 24 + opt;
    uint32_t irva = *(uint32_t *)(d + nt + 24 + 96 + 8);
    #define RVA2OFF(r) ({ uint32_t _o = 0; for (int _i = 0; _i < nsec; _i++){ uint32_t va = *(uint32_t *)(sh + 40 * _i + 12), sz = *(uint32_t *)(sh + 40 * _i + 16); \
        if ((r) >= va && (r) < va + sz){ _o = (r) - va + *(uint32_t *)(sh + 40 * _i + 20); break; } } _o; })
    for (uint32_t di = RVA2OFF(irva); di && di + 20 <= n; di += 20){
        uint32_t oft = *(uint32_t *)(d + di), nm = *(uint32_t *)(d + di + 12), ft = *(uint32_t *)(d + di + 16);
        if (!nm && !ft) break;
        uint32_t th = RVA2OFF(oft ? oft : ft);
        for (uint32_t k = 0; th && th + k + 4 <= n; k += 4){
            uint32_t v = *(uint32_t *)(d + th + k); if (!v) break;
            uint32_t slot = 0x400000u + ft + k, hle = *(uint32_t *)(g_mem + slot);
            if ((hle & 0xFFFFF000u) == 0xF0000000u && (hle & 0xFFFu) < 512 && !(v & 0x80000000u))
                snprintf(g_impname[hle & 0xFFFu], 40, "%s", (const char *)d + RVA2OFF(v) + 2);
        }
    }
    /* the exe's headers: Windows maps them at 0x400000, the port does not */
    uint32_t hs = *(uint32_t *)(d + nt + 24 + 60);
    memcpy(g_mem + 0x400000, d, hs); memcpy(g_memA + 0x400000, d, hs);
}
static uint32_t heap_alloc(Eng *e, uint32_t size){
    Heap *hp = e == &EA ? &g_heapA : &g_heapB; uint8_t *m = e == &EA ? g_memA : g_mem; uint8_t *bits = e == &EA ? g_cbitsA : g_cbits;
    uint32_t need = (size + 16u + 15u) & ~15u, p = hp->brk; hp->brk += need;
    commit(m, bits, p, need); memset(m + p, 0, need); memcpy(m + p, &size, 4);
    return p + 16;
}
static int game_import(xvm *vm, uint8_t *m, uint8_t *bits, Heap *hp, uint32_t addr){
    uint32_t i = addr & 0xFFFu; const char *n = g_impname[i];
    uint32_t r[XR__N]; xvm_get(vm, r);
    uint32_t esp = r[XR_ESP], ret = xvm_rd32(vm, esp), eax = 0; int na = -1;
    #define GA(k) xvm_rd32(vm, esp + 4u + 4u * (k))
    if (!strcmp(n, "HeapAlloc") || !strcmp(n, "HeapReAlloc")){
        int re = n[4] == 'R'; uint32_t size = re ? GA(3) : GA(2), old = re ? GA(2) : 0;
        uint32_t need = (size + 16u + 15u) & ~15u, p = hp->brk; hp->brk += need;
        commit(m, bits, p, need);
        memset(m + p, 0, need); memcpy(m + p, &size, 4);
        if (re && old){ uint32_t os = *(uint32_t *)(m + old - 16); memcpy(m + p + 16, m + old, os < size ? os : size); }
        eax = p + 16; na = re ? 4 : 3;
    }
    else if (!strcmp(n, "HeapFree")){ eax = 1; na = 3; }
    else if (!strcmp(n, "HeapSize")){ eax = *(uint32_t *)(m + GA(2) - 16); na = 3; }
    else if (!strcmp(n, "EnterCriticalSection") || !strcmp(n, "LeaveCriticalSection") || !strcmp(n, "InitializeCriticalSection")){ na = 1; }
    else if (!strcmp(n, "GetTickCount") || !strcmp(n, "timeGetTime")){ eax = (uint32_t)(g_now / 1000); na = 0; }
    else if (!strcmp(n, "QueryPerformanceCounter") || !strcmp(n, "QueryPerformanceFrequency")){
        int64_t v = n[5] == 'F' ? 1000000 : g_now; xvm_write(vm, GA(0), &v, 8); eax = 1; na = 1; }
    else if (!strcmp(n, "InterlockedIncrement") || !strcmp(n, "InterlockedDecrement")){
        uint32_t a = GA(0), v = xvm_rd32(vm, a) + (n[11] == 'I' ? 1u : 0xFFFFFFFFu); xvm_write(vm, a, &v, 4); eax = v; na = 1; }
    else if (!strcmp(n, "InterlockedExchange")){ uint32_t a = GA(0), v = GA(1); eax = xvm_rd32(vm, a); xvm_write(vm, a, &v, 4); na = 2; }
    else if (!strcmp(n, "GetCurrentThreadId")){ eax = 1; na = 0; }
    else if (!strcmp(n, "GetLastError")){ eax = 0; na = 0; }
    else if (!strcmp(n, "SetLastError") || !strcmp(n, "Sleep")){ na = 1; }
    else if (!strcmp(n, "IsBadReadPtr") || !strcmp(n, "IsBadWritePtr")){ eax = 0; na = 2; }      /* as the port's HLE: valid */
    else if (!strcmp(n, "TlsGetValue") || !strcmp(n, "TlsSetValue")){          /* the TEB's slots, as the port keeps them */
        uint32_t arr = xvm_rd32(vm, g_snap_hdr[10] + 0x2Cu), k = GA(0);
        if (n[3] == 'G') eax = xvm_rd32(vm, arr + 4u * k); else { uint32_t v = GA(1); xvm_write(vm, arr + 4u * k, &v, 4); eax = 1; }
        na = n[3] == 'G' ? 1 : 2; }
    if (na < 0){ say("        the game called %s (import %u): not emulated here\n", n[0] ? n : "?", i); return 0; }
    r[XR_EAX] = eax; r[XR_ESP] = esp + 4u + 4u * (uint32_t)na; r[XR_EIP] = ret;
    xvm_set(vm, r);
    return 1;
}
static int b_fault(void *u, xvm *vm, uint32_t eip, uint32_t addr, int write){
    (void)u; (void)eip;
    if (write == 8 && (addr & 0xFFFFF000u) == 0xF0000000u) return game_import(vm, g_mem, g_cbits, &g_heapB, addr);
    return 0;
}

/* ---- engine B: the Mac build, its guest calls under Unicorn ---------------------------- */
static int g_b_dead;
void dispatch_indirect(CPU *c, uint32_t fn){
    if (g_b_dead){ c->eax = 0; return; }
    uint32_t r[XR__N] = { c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi, 0, 0x202 };
    trace_add(&EB, EB.vm, fn, r);
    uint16_t sw, tag; uc_reg_read(xvm_uc(EB.vm), UC_X86_REG_FPSW, &sw); uc_reg_read(xvm_uc(EB.vm), UC_X86_REG_FPTAG, &tag);
    if (!xvm_call(EB.vm, fn, r)){
        say("  FAIL  mac build: game function 0x%08x stopped: %s\n", fn, xvm_why(EB.vm)); g_fail++; g_b_dead = 1;
    }
    /* the port restores its whole CPU after a guest call: the x87 stack too */
    uc_reg_write(xvm_uc(EB.vm), UC_X86_REG_FPSW, &sw); uc_reg_write(xvm_uc(EB.vm), UC_X86_REG_FPTAG, &tag);
    c->eax = r[XR_EAX]; c->edx = r[XR_EDX];
}

/* ---- engine B's platform layer (no sockets: the harness carries the datagrams) ---------- */
static rc_session *g_bs; static roam_plat_cfg g_bcfg; static int64_t g_b_next_tick;
static void b_send(void *u, uint32_t ip, uint16_t port, const uint8_t *b, int n){ (void)u; if ((ip >> 24) == 127) out_put(&EB, ip, port, b, n); }
static void b_bcast(void *u, uint16_t port, const uint8_t *b, int n){ (void)u; (void)port; (void)b; (void)n; }
void roam_game_event(int ev, int peer, const char *text);
static void b_event(void *u, int ev, int peer, const char *t){ (void)u; roam_game_event(ev, peer, t); }
int roam_plat_test_mode(void){ return 1; }
int roam_plat_start(const roam_plat_cfg *cfg, rc_session *s){
    g_bcfg = *cfg; g_bs = s;
    rc_io io = { 0, b_send, b_bcast, b_event }; s->io = io;
    fprintf(stderr, "[roam] UDP 127.0.0.1:%u open (test harness: loopback only)\n", cfg->port);
    g_b_next_tick = 0;
    return 0;
}
void roam_plat_stop(uint8_t reason){ if (g_bs) rc_shutdown(g_bs, reason); }
void roam_plat_lock(void){}
void roam_plat_unlock(void){}
uint32_t roam_plat_random(void){ return TEST_ID; }
int roam_plat_guard(int t, uint32_t ip, int bc){ return !t || (!bc && (ip >> 24) == 127); }
void roam_plat_get_stats(roam_plat_stats *o){ memset(o, 0, sizeof *o); }
int roam_plat_solid(void){ return -1; }
/* quick chat: none, as in the plugin (its HELLO flags stay the plugin's) */
int roam_plat_quickchat(void){ return 0; }
int roam_plat_say_pick(void){ return -1; }
int roam_plat_said(const char *json){ (void)json; return 1; }
int roam_plat_friend(const char *json){ (void)json; return 0; }
void roam_plat_say_marks(const uint32_t *ids, const float *xys, int n){ (void)ids; (void)xys; (void)n; }
uint32_t roam_plat_crews(uint32_t crews[RC_CREW_MAX], uint32_t rgb[RC_CREW_MAX]){ (void)crews; (void)rgb; return 0; }
uint32_t roam_plat_room(char name[64]){ (void)name; return 0; }
const char *roam_ip_str(uint32_t ip, char *b){ snprintf(b, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255u, (ip >> 8) & 255u, ip & 255u); return b; }
int roam_bcast_targets(const roam_if *ifs, int n, uint32_t *out, int max){ (void)ifs; (void)n; if (max) out[0] = 0xFFFFFFFFu; return max ? 1 : 0; }
int roam_plat_parse_peers(const char *list, int test_mode, uint16_t defport, uint32_t *ip, uint16_t *port, int max){
    int n = 0; const char *p = list;
    while (p && *p && n < max){
        while (*p == ' ' || *p == ',' || *p == ';') p++;
        unsigned a, b, c, d, pt = defport; int used = 0;
        if (sscanf(p, "%u.%u.%u.%u%n", &a, &b, &c, &d, &used) != 4) break;
        p += used; if (*p == ':'){ p++; pt = (unsigned)strtoul(p, (char **)&p, 10); }
        if (test_mode && a != 127) continue;
        ip[n] = a << 24 | b << 16 | c << 8 | d; port[n] = (uint16_t)pt; n++;
    }
    return n;
}
/* what roam_win.c's pump does in the plugin (NetThread = 0), the same here */
static void b_pump(void){
    if (!g_bs) return;
    Dg d; while (box_get(&EB.in, &d)) rc_receive(g_bs, d.ip, d.port, d.b, d.len, g_now);
    if (g_now >= g_b_next_tick){ rc_tick(g_bs, g_now); g_b_next_tick = g_now + 10000; }
}

/* ---- engine A: the plugin -------------------------------------------------------------- */
static aw32 W; static xpe PE; static uint32_t g_thunk;
static int64_t a_now(void *u){ (void)u; return g_now; }
static const char *g_aenv[16][2]; static int g_naenv;
static const char *a_getenv(void *u, const char *n){ (void)u; for (int i = 0; i < g_naenv; i++) if (!strcmp(g_aenv[i][0], n)) return g_aenv[i][1]; return 0; }
static int a_bind(void *u, uint32_t ip, uint16_t port){ (void)u; return ip == 0x7F000001u && port == ENGINE_PORT ? 0 : -1; }
static int a_send(void *u, uint32_t ip, uint16_t port, const uint8_t *b, int n){ (void)u; if ((ip >> 24) != 127) return -1; out_put(&EA, ip, port, b, n); return n; }
static int a_recv(void *u, uint8_t *b, int cap, uint32_t *ip, uint16_t *port){
    (void)u; Dg d; if (!box_get(&EA.in, &d)) return -1;
    int n = d.len < cap ? d.len : cap; memcpy(b, d.b, (size_t)n); *ip = d.ip; *port = d.port; return n; }
static int a_import(void *u, xvm *vm, int idx, const char *dll, const char *name, uint32_t esp, uint32_t *eax){
    (void)u; (void)idx; return aw32_import(&W, vm, dll, name, esp, eax); }
/* --fault: Windows' exception dispatch, cut down: the vectored handler gets
 * EXCEPTION_POINTERS on the faulting thread's stack, and its CONTEXT says
 * where to go on */
static int g_faults, g_fault_resumed;
static int a_fault(void *u, xvm *vm, uint32_t eip, uint32_t addr, int write){
    (void)u;
    if (write == 8 && (addr & 0xFFFFF000u) == 0xF0000000u) return game_import(vm, g_memA, g_cbitsA, &g_heapA, addr);
    g_faults++;
    if (!W.veh || g_faults > 3) return 0;
    uint32_t r[XR__N]; xvm_get(vm, r);
    uint32_t sp = (r[XR_ESP] - 0x1000u) & ~15u, ctx = sp + 0x100u, rec = sp + 0x40u, ptrs = sp + 0x30u;
    uint8_t z[0x400] = {0}; xvm_write(vm, sp, z, sizeof z);
    uint32_t er[6] = { 0xC0000005u, 0, 0, eip, 2, (uint32_t)write };
    xvm_write(vm, rec, er, sizeof er); xvm_write(vm, rec + 0x18, &addr, 4);
    uint32_t cx[] = { r[XR_EDI], r[XR_ESI], r[XR_EBX], r[XR_EDX], r[XR_ECX], r[XR_EAX], r[XR_EBP], eip, 0x1b, r[XR_EFL], r[XR_ESP], 0x23 };
    uint32_t flags = 0x1003Fu; xvm_write(vm, ctx, &flags, 4);
    xvm_write(vm, ctx + 0x9C, cx, sizeof cx);
    uint32_t pp[2] = { rec, ctx }; xvm_write(vm, ptrs, pp, 8);
    uint32_t args[2] = { 0, ptrs };                      /* return slot, EXCEPTION_POINTERS* */
    xvm_write(vm, sp, args, 8);
    uint32_t h[XR__N]; memcpy(h, r, sizeof h); h[XR_ESP] = sp;
    if (!xvm_call(vm, W.veh, h)){ say("  FAIL  vectored handler: %s\n", xvm_why(vm)); return 0; }
    if (h[XR_EAX] != 0xFFFFFFFFu){ say("  note  vectored handler: continue search (%d)\n", (int)h[XR_EAX]); return 0; }
    uint32_t back[12]; xvm_read(vm, ctx + 0x9C, back, sizeof back);
    r[XR_EDI] = back[0]; r[XR_ESI] = back[1]; r[XR_EBX] = back[2]; r[XR_EDX] = back[3]; r[XR_ECX] = back[4]; r[XR_EAX] = back[5];
    r[XR_EBP] = back[6]; r[XR_EIP] = back[7]; r[XR_EFL] = back[9]; r[XR_ESP] = back[10];
    xvm_set(vm, r);
    g_fault_resumed++;
    say("        fault at eip 0x%08x (%s 0x%08x): the plugin's handler resumes at 0x%08x\n", eip, write == 1 ? "write" : "read", addr, back[7]);
    return 1;
}

/* the 5 bytes at the hook site point at the plugin's thunk */
static uint32_t hook_target(void){
    uint8_t *s = g_memA + HOOK_SITE; int32_t rel; memcpy(&rel, s + 1, 4);
    return s[0] == 0xE8 ? HOOK_SITE + 5u + (uint32_t)rel : 0;
}
static int g_chain_hits; static uint32_t g_chain_page = 0x00FF0000u;
static int st_chain(void *u, xvm *vm, uint32_t a, uint32_t *r){ (void)u; (void)vm; (void)a; (void)r; g_chain_hits++; return 0; }
/* --unlimiter: count the detour's uses, then run its code (no return) */
static int g_unl_hits;
static void st_unl_hook(uc_engine *uc, uint64_t a, uint32_t sz, void *ud){ (void)uc; (void)a; (void)sz; (void)ud; g_unl_hits++; }


static int g_seh_bad; static uint32_t g_seh_was, g_seh_now;
static int a_frame(void){
    uint32_t t = hook_target();
    if (!t || t < ASI_BASE || t >= ASI_BASE + PE.size) return 0;     /* not hooked: the game's own call */
    uint32_t seh0 = xvm_rd32(EA.vm, g_snap_hdr[10]);                  /* FS:[0], the thread's SEH chain head */
    uint32_t r[XR__N] = { g_snap_hdr[2], g_snap_hdr[3], g_snap_hdr[4], g_snap_hdr[5], g_hook_esp - 4u, g_snap_hdr[7],
                          g_snap_hdr[8], g_snap_hdr[9], 0, 0x00000246u };
    uint32_t in[XR__N]; memcpy(in, r, sizeof in);
    uint32_t fl0 = 0x246u; uc_reg_write(xvm_uc(EA.vm), UC_X86_REG_EFLAGS, &fl0);
    if (!xvm_call(EA.vm, t, r)){ say("  FAIL  plugin frame %ld: %s (last game call 0x%08x)\n", g_frame, xvm_why(EA.vm), EA.ntr ? EA.tr[EA.ntr - 1].fn : 0); g_fail++; return -1; }
    static int reg_bad;
    int same = r[XR_ESP] == g_hook_esp;
    for (int i = 0; i < 8; i++) if (i != XR_ESP && r[i] != in[i]) same = 0;
    if ((r[XR_EFL] & 0x8D5u) != (0x246u & 0x8D5u)) same = 0;
    if (!same && !reg_bad++){ say("  FAIL  frame %ld: the thunk came back with other registers (esp %08x)\n", g_frame, r[XR_ESP]); g_fail++; }
    uint32_t seh1 = xvm_rd32(EA.vm, g_snap_hdr[10]);
    if (seh1 != seh0 && !g_seh_bad++){ g_seh_was = seh0; g_seh_now = seh1;
        say("  FAIL  frame %ld: FS:[0] was 0x%08x at the hook, 0x%08x after the plugin's step\n", g_frame, seh0, seh1); g_fail++; }
    return 1;
}

/* ---- comparisons ----------------------------------------------------------------------- */
static uint64_t g_cmp_bytes; static int g_mem_diffs, g_mem_cmps;
static int excluded(uint32_t a){
    if (a >= ASI_BASE && a < ASI_BASE + PE.size) return 1;                /* the plugin's own image */
    if (a >= g_hook_esp - 0x40000u && a < g_hook_esp + 0x40u) return 1;    /* the stack below the hook */
    if (a >= g_chain_page && a < g_chain_page + 0x4000u) return 1;
    if (a + 4u > HOOK_SITE && a < HOOK_SITE + 5u) return 1;               /* the plugin's patch itself */
    if (a + 4u > 0x61BCD0u && a < 0x61BCD6u) return 1;                    /* --unlimiter's detour */
    return 0;
}
static int mem_compare(int verbose){
    int nd = 0;
    g_mem_cmps++;
    for (uint32_t b = 0; b < (1u << 18); b++){
        int ca = (g_cbitsA[b >> 3] >> (b & 7)) & 1, cb = (g_cbits[b >> 3] >> (b & 7)) & 1;
        uint32_t lo = b << 14;
        if (!ca && !cb) continue;
        if (ca != cb){ if (!excluded(lo)){ if (nd++ < 8 && verbose) say("        block 0x%08x committed in one arena only\n", lo); } continue; }
        const uint8_t *x = g_memA + lo, *y = g_mem + lo;
        g_cmp_bytes += 0x4000;
        if (!memcmp(x, y, 0x4000)) continue;
        for (uint32_t i = 0; i < 0x4000; i += 4){
            if (!memcmp(x + i, y + i, 4) || excluded(lo + i)) continue;
            if (nd < 12 && verbose){ uint32_t va, vb; memcpy(&va, x + i, 4); memcpy(&vb, y + i, 4); say("        0x%08x: plugin %08x, mac %08x\n", lo + i, va, vb); }
            nd++;
        }
    }
    if (nd) g_mem_diffs++;
    return nd;
}
static int g_dg_frames_diff, g_dg_total, g_tr_diff, g_tr_total, g_hello_plat, g_hello_plat_bad;
/* the same datagram from both builds; a HELLO says which platform sent it
 * (byte 18: 2 = the Windows plugin, 1 = the Mac port), and its CRC follows */
static int dg_same(const Dg *a, const Dg *b){
    if (a->ip != b->ip || a->port != b->port || a->len != b->len) return 0;
    if (a->len > 23 && a->b[5] == RC_MSG_HELLO && b->b[5] == RC_MSG_HELLO){
        if (a->b[18] != RC_PLAT_WIN || b->b[18] != RC_PLAT_MAC){ g_hello_plat_bad++; return 0; }
        g_hello_plat++;
        return !memcmp(a->b, b->b, 18) && !memcmp(a->b + 19, b->b + 19, (size_t)a->len - 23);
    }
    return !memcmp(a->b, b->b, (size_t)a->len);
}
static void compare_frame(int memcheck){
    /* datagrams */
    int same = EA.nout == EB.nout;
    for (int i = 0; same && i < EA.nout; i++) same = dg_same(&EA.out[i], &EB.out[i]);
    g_dg_total += EA.nout;
    if (!same && g_dg_frames_diff++ < 3){
        say("  FAIL  frame %ld: datagrams differ (plugin %d, mac %d)\n", g_frame, EA.nout, EB.nout);
        for (int i = 0; i < EA.nout || i < EB.nout; i++){
            say("        #%d plugin:", i); for (int k = 0; i < EA.nout && k < EA.out[i].len && k < 40; k++) say("%02x", EA.out[i].b[k]);
            say("\n        #%d mac:   ", i); for (int k = 0; i < EB.nout && k < EB.out[i].len && k < 40; k++) say("%02x", EB.out[i].b[k]);
            say("\n");
        }
    }
    /* calls */
    int ts = EA.ntr == EB.ntr;
    for (int i = 0; ts && i < EA.ntr; i++){
        Call *x = &EA.tr[i], *y = &EB.tr[i];
        ts = x->fn == y->fn && x->ecx == y->ecx && x->edx == y->edx;
        for (int k = 0; ts && k < y->n && k < 5; k++) ts = x->a[k] == y->a[k];
    }
    g_tr_total += EB.ntr;
    if (!ts && g_tr_diff++ < 3){
        say("  FAIL  frame %ld: game calls differ (plugin %d, mac %d)\n", g_frame, EA.ntr, EB.ntr);
        for (int i = 0; i < 24 && (i < EA.ntr || i < EB.ntr); i++){
            if (i < EA.ntr) say("        plugin %08x ecx %08x edx %08x %08x %08x\n", EA.tr[i].fn, EA.tr[i].ecx, EA.tr[i].edx, EA.tr[i].a[0], EA.tr[i].a[1]);
            if (i < EB.ntr) say("        mac    %08x ecx %08x edx %08x %08x %08x\n", EB.tr[i].fn, EB.tr[i].ecx, EB.tr[i].edx, EB.tr[i].a[0], EB.tr[i].a[1]);
        }
    }
    if (memcheck){
        int nd = mem_compare(g_mem_diffs < 2);
        if (nd && g_mem_diffs <= 2) say("  FAIL  frame %ld: %d dwords of guest memory differ\n", g_frame, nd);
        if (nd) g_fail++;
    }
    if (!same || !ts) g_fail++;
}

/* the friend's car as the two engines left it */
static uint32_t opp_car(uint8_t *m, int k){
    uint32_t w = *(uint32_t *)(m + G_WORLD); if (!w) return 0;
    int first = (int)*(uint32_t *)(m + w + 0x14u);
    return *(uint32_t *)(m + w + 0x1cu + 4u * (uint32_t)(first + k));
}
static void car_report(const char *who, uint8_t *m, uint32_t car){
    const float *p = (const float *)(m + car + 0x60), *dp = (const float *)(m + car + 0x50), *f = (const float *)(m + car + 0x90);
    say("        %-6s car 0x%08x: position %.3f %.3f %.3f, drawn at %.3f %.3f %.3f, forward %.3f %.3f %.3f, heading 0x%04x, speed %.2f, rpm %.0f, driver %u, physics %u\n",
        who, car, p[0], p[1], p[2], dp[0], dp[1], dp[2], f[0], f[1], f[2], *(uint16_t *)(m + car + 0x40c), *(float *)(m + car + 0x42c),
        *(float *)(m + car + 0x440), *(uint32_t *)(m + car + 0x24), *(uint32_t *)(m + car + 0x28));
}

int main(int argc, char **argv){
    int frames = 1800, mode_fault = 0, mode_badexe = 0, mode_chain = 0, mode_unl = 0, ai = 1;
    const int fault_frame = 300;
    for (; ai < argc && argv[ai][0] == '-'; ai++){
        if (!strcmp(argv[ai], "--frames") && ai + 1 < argc) frames = atoi(argv[++ai]);
        else if (!strcmp(argv[ai], "--fault")) mode_fault = 1;
        else if (!strcmp(argv[ai], "--faultself")) mode_fault = 2;
        else if (!strcmp(argv[ai], "--faultseh")) mode_fault = 3;
        else if (!strcmp(argv[ai], "--badexe")) mode_badexe = 1;
        else if (!strcmp(argv[ai], "--chain")) mode_chain = 1;
        else if (!strcmp(argv[ai], "--unlimiter")) mode_unl = 1;
    }
    if (argc - ai < 3){ fprintf(stderr, "usage: asi_harness [--frames N] [--fault|--faultself|--faultseh|--badexe|--chain|--unlimiter] snap.bin cfg.bin outdir\n"); return 2; }
    const char *snap = argv[ai], *cfgp = argv[ai + 1], *od = argv[ai + 2];
    char p[700];
    g_out = stdout;
    snprintf(p, sizeof p, "%s/mac.log", od);
    if (!freopen(p, "w", stderr)){ perror(p); return 2; }
    setvbuf(stderr, 0, _IOLBF, 0);
    {   /* no NFSU2_* of the caller's: both builds see only what is set here */
        char names[64][128]; int nn = 0;
        for (char **e = *_NSGetEnviron(); *e && nn < 64; e++)
            if (!strncmp(*e, "NFSU2_", 6) && strchr(*e, '=')) snprintf(names[nn++], 128, "%.*s", (int)(strchr(*e, '=') - *e), *e);
        for (int i = 0; i < nn; i++) unsetenv(names[i]);
    }
    /* the same options for both: the plugin from its .ini (and two test aids
     * from its environment), the Mac build from the environment */
    setenv("NFSU2_ROAM", "1", 1); setenv("NFSU2_ROAM_NAME", "Engine", 1); setenv("NFSU2_ROAM_PORT", "47210", 1);
    setenv("NFSU2_ROAM_PEERS", "127.0.0.1:47391", 1); setenv("NFSU2_ROAM_DISCOVERY", "0", 1); setenv("NFSU2_ROAM_MAX_CARS", "3", 1);
    setenv("NFSU2_ROAM_HARNESS", "1", 1);
    snprintf(W.ini_path, sizeof W.ini_path, "%s/NFSU2FreeRoam.ini", od);
    FILE *ini = fopen(W.ini_path, "w");
    fprintf(ini, "[FreeRoam]\nEnable = 1\nName = Engine   ; the same name as the Mac build\nPort = 47210\nPeers = 127.0.0.1:47391\n"
                 "Discovery = 0\nMaxCars = 3\nNetThread = 1\nLog = 1\nTestMode = 1\n");
    fclose(ini);
    g_aenv[g_naenv][0] = "NFSU2_ROAM_TEST_ID"; g_aenv[g_naenv++][1] = "1D0E5E1D";
    if (mode_fault == 3){ g_aenv[g_naenv][0] = "NFSU2_ROAM_TEST_FAULT_SEH"; g_aenv[g_naenv++][1] = "300"; }
    else if (mode_fault){ g_aenv[g_naenv][0] = "NFSU2_ROAM_TEST_FAULT"; g_aenv[g_naenv++][1] = mode_fault == 1 ? "300" : "-300"; }
    snprintf(W.module_path, sizeof W.module_path, "C:\\Games\\NFSU2\\scripts\\NFSU2FreeRoam.asi");
    snprintf(W.file_dir, sizeof W.file_dir, "%s", od);
    W.now_us = a_now; W.getenv = a_getenv; W.sock_bind = a_bind; W.sock_send = a_send; W.sock_recv = a_recv;

    FILE *cf = fopen(cfgp, "rb");
    if (!cf){ perror(cfgp); return 2; }
    g_cfg_len = (int)fread(g_cfg, 1, sizeof g_cfg, cf); fclose(cf);
    g_mem = arena(); g_memA = arena();
    if (snap_load(snap)) return 2;
    game_imports(getenv("ROAM_EXE") ? getenv("ROAM_EXE") : "../../gamedata/SPEED2.EXE");
    say("asi_harness: snapshot %s: %u blocks (%u MB), hook esp 0x%08x; friend's car look %s (%d bytes)\n", snap, g_nblocks,
        g_nblocks >> 6, g_hook_esp, cfgp, g_cfg_len);
    uint32_t me = *(uint32_t *)(g_mem + *(uint32_t *)(g_mem + G_PLAYER1) + 4);
    memcpy(g_ctr, g_mem + me + 0x60, 12); g_ctr[0] += 25.f;
    say("        the player's car 0x%08x at %.1f %.1f %.1f; the friend drives a 12 m circle around %.1f %.1f\n", me,
        *(float *)(g_mem + me + 0x60), *(float *)(g_mem + me + 0x64), *(float *)(g_mem + me + 0x68), g_ctr[0], g_ctr[1]);

    xvm_cfg ca = { &EA, g_memA, committedA, g_snap_hdr[10], (uint16_t)g_snap_hdr[11], a_import, 0, 0, 0, a_fault };
    xvm_cfg cb = { &EB, g_mem, committedB, g_snap_hdr[10], (uint16_t)g_snap_hdr[11], 0, 0, 0, 0, b_fault };
    EA.vm = xvm_new(&ca); EB.vm = xvm_new(&cb);
    add_stubs(EA.vm); add_stubs(EB.vm);
    if (mode_chain){
        /* "another mod": a call at 0x581475 to a ret of its own */
        commit(g_memA, g_cbitsA, g_chain_page, 0x4000); g_memA[g_chain_page] = 0xC3;
        int32_t rel = (int32_t)(g_chain_page - (HOOK_SITE + 5u)); g_memA[HOOK_SITE] = 0xE8; memcpy(g_memA + HOOK_SITE + 1, &rel, 4);
        xvm_stub(EA.vm, g_chain_page, st_chain);
    }
    if (mode_unl){
        /* "Unlimiter": jmp to a copy of the first instruction (sub esp, 0x88),
         * then back to 0x61BCD6; the calls must reach it */
        uint32_t tr = g_chain_page + 0x100u;
        commit(g_memA, g_cbitsA, g_chain_page, 0x4000);
        memcpy(g_memA + tr, g_memA + 0x61BCD0u, 6);
        g_memA[tr + 6] = 0xE9; int32_t back = (int32_t)(0x61BCD6u - (tr + 11u)); memcpy(g_memA + tr + 7, &back, 4);
        g_memA[0x61BCD0u] = 0xE9; int32_t to = (int32_t)(tr - 0x61BCD5u); memcpy(g_memA + 0x61BCD1u, &to, 4); g_memA[0x61BCD5u] = 0x90;
        uc_hook hh; uc_hook_add(xvm_uc(EA.vm), &hh, UC_HOOK_CODE, (void *)st_unl_hook, 0, tr, tr);
    }
    uint32_t bad_va = 0x005F6CDAu; uint8_t bad_old = g_memA[bad_va];
    if (mode_badexe) g_memA[bad_va] ^= 0x01;            /* inside FN_CAR_SECTION, not its first byte */

    /* ---- load the plugin ---- */
    say("1. Loading NFSU2FreeRoam.asi at 0x%08x (it was linked for 0x10000000)\n", ASI_BASE);
    char asi[700], map[700];
    const char *root = getenv("ROAM_ROOT") ? getenv("ROAM_ROOT") : ".";
    snprintf(asi, sizeof asi, "%s/out/win/dbg/NFSU2FreeRoam.asi", root); snprintf(map, sizeof map, "%s/out/win/NFSU2FreeRoam.map", root);
    static uint8_t img_before[0x532000];
    memcpy(img_before, g_memA + 0x400000, sizeof img_before);
    if (xpe_load(&PE, EA.vm, asi, map, ASI_BASE, commitA)){ say("  FAIL  cannot load %s\n", asi); return 1; }
    xvm_bind_pe(EA.vm, &PE);
    check(PE.nreloc > 1000 && PE.nimp > 30, "mapped: %u bytes, %d relocations applied, %d imports bound, %d exports, %d map symbols",
          PE.size, PE.nreloc, PE.nimp, PE.nexp, PE.nsym);
    uint32_t ep[XR__N] = {0}; ep[XR_ESP] = g_hook_esp - 0x2000u;
    uint32_t dm[4] = { 0, ASI_BASE, 1, 0 }; xvm_write(EA.vm, ep[XR_ESP], dm, 16);
    xvm_trace(EA.vm, ASI_BASE, ASI_BASE + PE.size, TEXT_LO, TEXT_HI, on_trace_A);
    int ok = xvm_call(EA.vm, PE.entry, ep);
    check(ok && ep[XR_EAX] == 1 && ep[XR_ESP] == g_hook_esp - 0x2000u + 16u, "DllMain(PROCESS_ATTACH) returned TRUE, popped its 3 arguments%s%s",
          ok ? "" : ": ", ok ? "" : xvm_why(EA.vm));
    uint32_t ia[XR__N] = {0}; ia[XR_ESP] = g_hook_esp - 0x2000u;
    ok = xvm_call(EA.vm, xpe_export(&PE, "InitializeASI"), ia);
    check(ok, "InitializeASI returned%s%s", ok ? "" : ": ", ok ? "" : xvm_why(EA.vm));
    int nchg = 0; uint32_t first = 0, last = 0;
    if (mode_badexe) g_memA[bad_va] = bad_old ^ 0x01;
    for (uint32_t i = 0; i < sizeof img_before; i++) if (img_before[i] != g_memA[0x400000 + i]){ if (!nchg) first = 0x400000 + i; last = 0x400000 + i; nchg++; }
    uint32_t tgt = hook_target();
    if (mode_badexe){
        check(nchg == 0 || (nchg == 1 && first == bad_va), "with one byte of FN_CAR_SECTION changed the plugin changed nothing in the exe (%d bytes)", nchg - (first == bad_va));
        check(!(tgt >= ASI_BASE && tgt < ASI_BASE + PE.size), "the frame hook is not installed");
        snprintf(p, sizeof p, "%s/NFSU2FreeRoam.log", od);
        FILE *lg = fopen(p, "r"); char line[600]; int said = 0;
        while (lg && fgets(line, sizeof line, lg)) if (strstr(line, "different code") || strstr(line, "stays OFF")){ say("        log: %s", line + 13); said++; }
        if (lg) fclose(lg);
        check(said >= 2, "the log says which function differs and that the plugin stays off");
        say("asi_harness: %s\n", g_fail ? "FAIL" : "PASS"); return g_fail ? 1 : 0;
    }
    if (mode_unl){                                      /* the detour's 6 bytes are not the plugin's */
        nchg = 0; first = last = 0;
        for (uint32_t i = 0; i < sizeof img_before; i++)
            if (img_before[i] != g_memA[0x400000 + i] && (0x400000 + i < 0x61BCD0u || 0x400000 + i >= 0x61BCD6u)){ if (!nchg) first = 0x400000 + i; last = 0x400000 + i; nchg++; }
        snprintf(p, sizeof p, "%s/NFSU2FreeRoam.log", od);
        FILE *lg = fopen(p, "r"); char line[600]; int said = 0;
        while (lg && fgets(line, sizeof line, lg)) if (strstr(line, "replaced by another mod")){ say("        log: %s", line + 13); said++; }
        if (lg) fclose(lg);
        check(said == 1, "the exe check accepts UpdatePartsEnabled replaced by a jump");
    }
    check(nchg >= 1 && nchg <= 5 && first >= HOOK_SITE && last < HOOK_SITE + 5,
          "the exe image changed in exactly the frame hook's bytes (%d bytes, 0x%08x..0x%08x)", nchg, first, last);
    g_thunk = xpe_sym(&PE, "roam_frame_thunk");
    check(tgt == g_thunk && g_thunk, "0x581475 now calls the plugin's frame thunk 0x%08x", tgt);
    if (mode_chain){
        uint32_t chain = xvm_rd32(EA.vm, xpe_sym(&PE, "g_roam_chain"));
        check(chain == g_chain_page, "the other mod's call target 0x%08x is kept as the chain (0x%08x)", g_chain_page, chain);
    }
    EA.ntr = 0;
    fake_init(&EA, 0xFA4E0001u); fake_init(&EB, 0xFA4E0001u);
    long leave_at = mode_fault ? 0 : frames / 2, back_at = mode_fault ? 0 : frames * 7 / 10;

    /* ---- frames ---- */
    if (mode_fault) frames = fault_frame + 200;
    say("2. %d frames, both builds side by side%s\n", frames, mode_fault ? ", the plugin alone (a fault in frame 300)" :
        "; the friend leaves half-way and comes back at 70 %");
    CPU cpu; memset(&cpu, 0, sizeof cpu);
    float last_pos[3] = {0};
    int shown_from = -1;
    for (long k = 1; k <= frames; k++){
        g_frame = k; g_now += FRAME_US;
        float wdt = (float)FRAME_US / 1e6f;
        memcpy(g_memA + G_WORLDDT, &wdt, 4); memcpy(g_mem + G_WORLDDT, &wdt, 4);
        EA.nout = EB.nout = 0; EA.ntr = EB.ntr = 0;
        if (k == leave_at){ rc_shutdown(&EA.fk, 0); rc_shutdown(&EB.fk, 0); EA.gone = EB.gone = 1; }
        if (k == back_at){ fake_init(&EA, 0xFA4E0002u); fake_init(&EB, 0xFA4E0002u); }
        fake_step(&EA, last_pos); fake_step(&EB, 0);
        if (k == leave_at - 1 || k == back_at - 1 || k == back_at + 60){
            uint32_t c0 = opp_car(g_memA, 0);
            if (c0) say("        frame %ld: the friend's car at %.1f %.1f %.1f%s\n", k, *(float *)(g_memA + c0 + 0x60), *(float *)(g_memA + c0 + 0x64),
                        *(float *)(g_memA + c0 + 0x68), k == leave_at - 1 ? " (the friend leaves next)" : k == back_at - 1 ? " (parked; the friend comes back next)" : "");
        }
        if (a_frame() < 0) break;
        if (!mode_fault){
            b_pump();
            memset(&cpu, 0, sizeof cpu);
            cpu.eax = g_snap_hdr[2]; cpu.ecx = g_snap_hdr[3]; cpu.edx = g_snap_hdr[4]; cpu.ebx = g_snap_hdr[5]; cpu.esp = g_hook_esp;
            cpu.ebp = g_snap_hdr[7]; cpu.esi = g_snap_hdr[8]; cpu.edi = g_snap_hdr[9]; cpu.fs_base = g_snap_hdr[10]; cpu.fcw = (uint16_t)g_snap_hdr[11];
            roam_game_frame(&cpu);
        }
        fake_take(&EA); fake_take(&EB);
        if (!mode_fault) compare_frame(k <= 400 || k % 25 == 0 || k == frames);
        uint32_t car = opp_car(g_memA, 0);
        if (car && shown_from < 0 && *(float *)(g_memA + car + 0x68) > -1000.f) shown_from = (int)k;
    }
    if (mode_fault){
        say("3. The fault\n");
        check(shown_from > 0 && shown_from < fault_frame, "the friend's car was shown from frame %d", shown_from);
        check(g_faults == 1 && g_fault_resumed == 1, "one fault, resumed by the plugin's vectored handler (in %s)",
              mode_fault == 1 ? "a game function" : mode_fault == 3 ? "a game function inside its SEH frame" : "the plugin's code");
        check(g_seh_bad == 0, "FS:[0] (the SEH chain head) as it was at the hook after every frame, the fault's included%s", g_seh_bad ? "" : "");
        snprintf(p, sizeof p, "%s/NFSU2FreeRoam.log", od);
        FILE *lg = fopen(p, "r"); char line[700]; int said = 0;
        while (lg && fgets(line, sizeof line, lg)) if (strstr(line, "FAULT")){ say("        log: %s", line + 13); said++; }
        if (lg) fclose(lg);
        check(said == 1, "the log reports it once");
        check(EA.byes >= 1 && EA.bye_frame == fault_frame && EA.sent_after_bye == 0, "BYE sent to the friend in that frame (%d), nothing after it", EA.byes);
        int est = 0; for (int i = 0; i < RC_TABLE; i++) if (EA.fk.peer[i].state == RC_PS_EST) est++;
        check(est == 0, "the friend dropped the plugin's player");
        check(g_fail == 0, "frames %d..%d ran through the thunk with the plugin off, the game's registers and flags restored every time", fault_frame + 1, frames);
        say("asi_harness: %s\n", g_fail ? "FAIL" : "PASS"); return g_fail ? 1 : 0;
    }
    say("3. Results\n");
    check(g_dg_frames_diff == 0, "datagrams: %d sent by each build, identical in every frame", g_dg_total);
    check(g_hello_plat > 0 && g_hello_plat_bad == 0, "HELLO platform byte: 2 (Windows plugin) from the plugin, 1 (Mac) from the Mac build, in all %d HELLOs; "
          "the rest of each HELLO identical", g_hello_plat);
    check(g_seh_bad == 0, "FS:[0] (the SEH chain head) as it was at the hook after every frame");
    check(g_tr_diff == 0, "game calls: %d by each build, the same functions, ecx, edx and arguments in the same order", g_tr_total);
    check(g_mem_diffs == 0, "guest memory: identical after %d of %d frames (%.1f GB compared)", g_mem_cmps, frames, g_cmp_bytes / 1e9);
    uint32_t ca0 = opp_car(g_memA, 0), cb0 = opp_car(g_mem, 0);
    say("        the friend's last position %.3f %.3f %.3f\n", last_pos[0], last_pos[1], last_pos[2]);
    if (ca0) car_report("plugin", g_memA, ca0);
    if (cb0) car_report("mac", g_mem, cb0);
    float *pa = (float *)(g_memA + ca0 + 0x60);
    float dx = pa[0] - last_pos[0], dy = pa[1] - last_pos[1];
    check(ca0 && EA.setrides >= 1 && dx * dx + dy * dy < 9.f * 9.f, "the friend's car (opponent car 0) wears the friend's ride (SetRideInfo %d time(s)) and "
          "follows the friend: %.1f m from its newest position (100 ms behind at 14 m/s)", EA.setrides, (double)sqrtf(dx * dx + dy * dy));
    check(shown_from > 0, "the friend's car was first shown in frame %d", shown_from);
    if (mode_unl) check(g_unl_hits > 0, "the plugin's calls of UpdatePartsEnabled went through the replacement (%d)", g_unl_hits);
    if (mode_chain) check(g_chain_hits == frames, "the other mod's hook ran after the plugin's step in every frame (%d of %d)", g_chain_hits, frames);
    /* the logs: the lines roam_game.c writes must be the same */
    snprintf(p, sizeof p, "%s/NFSU2FreeRoam.log", od); FILE *la = fopen(p, "r");
    fflush(stderr); snprintf(p, sizeof p, "%s/mac.log", od); FILE *lb = fopen(p, "r");
    char x[800], y[800]; int nl = 0, nsame = 0;
    char ga[400][300]; int nga = 0;
    while (la && fgets(x, sizeof x, la)) if (strlen(x) > 13 && !strncmp(x + 13, "[roam] f=", 9) && nga < 400){ x[strcspn(x, "\r\n")] = 0; snprintf(ga[nga++], 300, "%s", x + 13); }
    int k2 = 0;
    while (lb && fgets(y, sizeof y, lb)){
        if (strncmp(y, "[roam] f=", 9)) continue;
        y[strcspn(y, "\r\n")] = 0;
        if (strstr(y, "stats:") || strstr(y, "frame cost")) continue;
        nl++;
        while (k2 < nga && (strstr(ga[k2], "stats:") || strstr(ga[k2], "frame cost"))) k2++;
        if (k2 < nga && !strncmp(ga[k2], y, 299)) nsame++;
        else if (nl - nsame <= 3) say("        log differs: mac \"%s\" / plugin \"%s\"\n", y, k2 < nga ? ga[k2] : "(none)");
        k2++;
    }
    if (la) fclose(la);
    if (lb) fclose(lb);
    check(nl > 3 && nsame == nl, "the game side's log lines are the same in both builds (%d lines)", nl);
    say("asi_harness: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
