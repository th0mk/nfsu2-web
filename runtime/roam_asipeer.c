/* roam_asipeer.c -- a "virtual Windows friend" (test tooling, never in the
 * player's build): the real Windows plugin NFSU2FreeRoam.asi plays the
 * free-roam multiplayer inside a port instance, as if this were the PC game.
 *
 * Built into out/nfsu2_asipeer only (ASIPEER=1 UNICORN_DIR=... ./link.sh orig);
 * on with NFSU2_ROAM_ASI=<path to out/win/dbg/NFSU2FreeRoam.asi>.  Then:
 *  - the plugin's 32-bit x86 code runs under Unicorn (tests/asi_vm.c) on the
 *    port's own guest memory: the exe's headers are put at 0x400000 first
 *    (Windows maps them; the port does not), the plugin is loaded at a free
 *    guest address, relocated, and its DllMain and InitializeASI run, which
 *    check the exe and patch the call at 0x581475, as under Ultimate ASI
 *    Loader;
 *  - at the main-loop hook (roam.c, HOOK_SITE 0x581475) the call there is
 *    followed: it points into the plugin, so its frame thunk runs; every call
 *    it makes into the game's code (.text, mapped without execute rights) goes
 *    to the port's lifted function of that address (dispatch_indirect), with
 *    the registers, the x87 stack and the SSE registers carried across, the
 *    same functions the Mac build calls; the thunk's last jump (to 0x4022C0)
 *    is such a call too;
 *  - its Win32 imports are tests/asi_win32.c: the clock is the host's
 *    monotonic clock (the same as the Mac side's, so pose logs compare), its
 *    files go into the run directory, its UDP socket is a host socket that
 *    may only bind 127.0.0.1 and only send to 127/8 (anything else is refused
 *    and logged as BLOCKED), no thread (the plugin then polls from its frame
 *    hook);
 *  - the Mac build's own game side (roam_game.c) is off in this instance.
 * Options: the plugin reads its NFSU2_ROAM_* settings from the environment
 * (tools/roam_multi.sh sets them per instance) and a generated .ini
 * (NetThread = 0).  Script commands "roam asi status|waitcars N [tmo]|
 * follow K [back]" look at the game only (the friends' cars are the inert
 * cars outside the parking spot).
 */
#include "cpu.h"
#include "asi_vm.h"
#include "asi_win32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

void dispatch_indirect(CPU *c, uint32_t addr);
long autotest_frame(void);
void mem_commit(uint32_t addr, uint32_t size);

#define HOOK_SITE  0x00581475u
#define TEXT_LO    0x00401000u
#define TEXT_HI    0x00783000u
#define G_WORLD    0x00890080u
#define G_PLAYER1  SPLIT_PLAYERS_INDEX
#define PARK_Z     (-123456.f)

static int      g_state;                 /* 0 not yet, 1 running, -1 off */
void roam_asipeer_report(void);
static xvm     *g_vm;
static xpe      g_pe;
static aw32     g_w;
static CPU     *g_c;                     /* the guest CPU at the hook, during the plugin's frame */
static int      g_fd = -1;
static uint64_t g_bridged, g_frames, g_blocked, g_ns;
static uint32_t g_thunk;

static int64_t host_now(void *u){ (void)u; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000; }
static const char *peer_getenv(void *u, const char *n){ (void)u; return !strncmp(n, "NFSU2_", 6) ? getenv(n) : 0; }
static int committed(void *u, uint32_t a){ (void)u; return mem_is_committed(a); }
static void commit(void *u, uint32_t a, uint32_t n){ (void)u; mem_commit(a, n); }
/* the socket: loopback only, whatever the plugin asks for */
static int s_bind(void *u, uint32_t ip, uint16_t port){
    (void)u;
    if (ip != 0x7F000001u){ g_blocked++; fprintf(stderr, "[asipeer] BLOCKED bind %08x:%u (loopback only)\n", ip, port); return -1; }
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_len = sizeof sa;
    sa.sin_port = htons(port); sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0){ fprintf(stderr, "[asipeer] bind 127.0.0.1:%u: %s\n", port, strerror(errno)); close(fd); return -1; }
    g_fd = fd;
    fprintf(stderr, "[asipeer] the plugin's UDP socket: host 127.0.0.1:%u\n", port);
    return 0;
}
static int s_send(void *u, uint32_t ip, uint16_t port, const uint8_t *b, int n){
    (void)u;
    if ((ip >> 24) != 127 || g_fd < 0){ g_blocked++; fprintf(stderr, "[asipeer] BLOCKED sendto %u.%u.%u.%u (loopback only)\n", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255); return -1; }
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_len = sizeof sa;
    sa.sin_port = htons(port); sa.sin_addr.s_addr = htonl(ip);
    return (int)sendto(g_fd, b, (size_t)n, 0, (struct sockaddr *)&sa, sizeof sa);
}
static int s_recv(void *u, uint8_t *b, int cap, uint32_t *ip, uint16_t *port){
    (void)u;
    if (g_fd < 0) return -1;
    struct sockaddr_in sa; socklen_t sl = sizeof sa;
    ssize_t n = recvfrom(g_fd, b, (size_t)cap, 0, (struct sockaddr *)&sa, &sl);
    if (n < 0) return -1;
    *ip = ntohl(sa.sin_addr.s_addr); *port = ntohs(sa.sin_port);
    return (int)n;
}
static int imp(void *u, xvm *vm, int idx, const char *dll, const char *name, uint32_t esp, uint32_t *eax){
    (void)u; (void)idx; return aw32_import(&g_w, vm, dll, name, esp, eax); }

/* ---- a call from the plugin into the game: the port's lifted function ---------------- */
static void uc_to_cpu(xvm *vm, CPU *c){
    uint32_t r[XR__N]; xvm_get(vm, r);
    c->eax = r[XR_EAX]; c->ecx = r[XR_ECX]; c->edx = r[XR_EDX]; c->ebx = r[XR_EBX]; c->esp = r[XR_ESP];
    c->ebp = r[XR_EBP]; c->esi = r[XR_ESI]; c->edi = r[XR_EDI]; c->eip = r[XR_EIP];
    eflags_set(c, (r[XR_EFL] & ~0x00240000u) | (c->sysfl & 0x00240000u));
    uc_engine *uc = xvm_uc(vm);
    uint16_t sw, cw; uc_reg_read(uc, UC_X86_REG_FPSW, &sw); uc_reg_read(uc, UC_X86_REG_FPCW, &cw);
    c->top = (sw >> 11) & 7; c->fsw = (uint16_t)(sw & ~0x3800u); c->fcw = cw;
    for (int i = 0; i < 8; i++){ uint8_t f[10]; uc_reg_read(uc, UC_X86_REG_FP0 + i, f); c->st[i] = f80_load(f); }   /* physical registers, as the port keeps them */
    for (int i = 0; i < 8; i++) uc_reg_read(uc, UC_X86_REG_XMM0 + i, c->xmm[i].b);
}
static void cpu_to_uc(xvm *vm, const CPU *c){
    uint32_t r[XR__N] = { c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi, c->eip, eflags_get(c) & ~0x00240000u };
    xvm_set(vm, r);
    uc_engine *uc = xvm_uc(vm);
    uint16_t sw = (uint16_t)((c->fsw & ~0x3800u) | ((c->top & 7) << 11)), cw = c->fcw, tag = 0;
    uc_reg_write(uc, UC_X86_REG_FPSW, &sw); uc_reg_write(uc, UC_X86_REG_FPCW, &cw); uc_reg_write(uc, UC_X86_REG_FPTAG, &tag);
    for (int i = 0; i < 8; i++){ uint8_t f[10]; f80_store(f, c->st[i]); uc_reg_write(uc, UC_X86_REG_FP0 + i, f); }
    for (int i = 0; i < 8; i++) uc_reg_write(uc, UC_X86_REG_XMM0 + i, c->xmm[i].b);
}
static int bridge(void *u, xvm *vm, uint32_t target){
    (void)u;
    CPU *c = g_c;
    uc_to_cpu(vm, c);
    dispatch_indirect(c, target);       /* returns with the return address popped into c->eip */
    cpu_to_uc(vm, c);
    g_bridged++;
    return 1;
}
static int on_fault(void *u, xvm *vm, uint32_t eip, uint32_t addr, int write){
    (void)u; (void)vm;
    fprintf(stderr, "[asipeer] the plugin's code faulted: %s 0x%08x at 0x%08x\n", write == 1 ? "write" : write == 8 ? "fetch" : "read", addr, eip);
    return 0;
}

static void start(CPU *c){
    g_state = -1;
    const char *asi = getenv("NFSU2_ROAM_ASI");
    char map[600]; snprintf(map, sizeof map, "%s", asi);
    char *sl = strrchr(map, '/');
    if (sl && sl - map >= 4 && !strncmp(sl - 4, "/dbg", 4)){ *(sl - 4) = 0; strcat(map, "/NFSU2FreeRoam.map"); } else map[0] = 0;
    /* the exe's headers, where Windows has them */
    const char *data = getenv("NFSU2_DATA");
    char exe[600]; snprintf(exe, sizeof exe, "%s/SPEED2.EXE", data ? data : ".");
    FILE *f = fopen(exe, "rb");
    if (!f){ fprintf(stderr, "[asipeer] cannot read %s (NFSU2_DATA) for its headers: off\n", exe); return; }
    uint8_t h[0x1000]; size_t n = fread(h, 1, sizeof h, f); fclose(f);
    if (n != sizeof h){ return; }
    mem_commit(0x400000u, 0x1000u); memcpy(g_mem + 0x400000u, h, 0x1000u);
    /* a free guest range for the plugin: above modheap.c's heaps of a lifted
     * DLL (server.dll, 0x01000000-0x0F000000, committed only when LAN hosting
     * starts) and below the guest heap (heap.c, 0x10000000-0x60000000) */
    uint32_t base = 0;
    for (uint32_t b = 0x0F000000u; b < 0x10000000u && !base; b += 0x00100000u){
        int used = 0; for (uint32_t a = b; a < b + 0x00100000u; a += 0x4000u) if (mem_is_committed(a)){ used = 1; break; }
        if (!used) base = b;
    }
    if (!base){ fprintf(stderr, "[asipeer] no free guest range for the plugin: off\n"); return; }
    snprintf(g_w.ini_path, sizeof g_w.ini_path, "NFSU2FreeRoam.ini");
    FILE *ini = fopen(g_w.ini_path, "w");
    if (ini){ fprintf(ini, "[FreeRoam]\nEnable = 1\nNetThread = 0\nLog = 1\n"); fclose(ini); }
    snprintf(g_w.module_path, sizeof g_w.module_path, "C:\\NFSU2\\scripts\\NFSU2FreeRoam.asi");
    snprintf(g_w.file_dir, sizeof g_w.file_dir, ".");
    g_w.now_us = host_now; g_w.getenv = peer_getenv; g_w.sock_bind = s_bind; g_w.sock_send = s_send; g_w.sock_recv = s_recv;
    xvm_cfg cf = { 0, g_mem, committed, c->fs_base, c->fcw, imp, bridge, TEXT_LO, TEXT_HI, on_fault };
    g_vm = xvm_new(&cf);
    if (!g_vm || xpe_load(&g_pe, g_vm, asi, map[0] ? map : 0, base, commit)){ fprintf(stderr, "[asipeer] cannot load %s: off\n", asi); return; }
    xvm_bind_pe(g_vm, &g_pe);
    fprintf(stderr, "[asipeer] %s loaded at 0x%08x (%u bytes, %d relocations, %d imports)\n", asi, base, g_pe.size, g_pe.nreloc, g_pe.nimp);
    g_c = c;
    CPU save = *c;
    uint32_t r[XR__N] = {0};
    r[XR_ESP] = (c->esp - 0x1000u) & ~15u;
    uint32_t a[4] = { 0, base, 1, 0 }; memcpy(g_mem + r[XR_ESP], a, 16);
    cpu_to_uc(g_vm, c);
    if (!xvm_call(g_vm, g_pe.entry, r) || r[XR_EAX] != 1){ fprintf(stderr, "[asipeer] DllMain failed: %s\n", xvm_why(g_vm)); *c = save; return; }
    r[XR_ESP] = (c->esp - 0x1000u) & ~15u;
    if (!xvm_call(g_vm, xpe_export(&g_pe, "InitializeASI"), r)){ fprintf(stderr, "[asipeer] InitializeASI failed: %s\n", xvm_why(g_vm)); *c = save; return; }
    *c = save;
    g_thunk = xpe_sym(&g_pe, "roam_frame_thunk");
    atexit(roam_asipeer_report);
    fprintf(stderr, "[asipeer] DllMain and InitializeASI done; 0x581475: %02x %02x %02x %02x %02x (the plugin's thunk 0x%08x); "
                    "the Mac build's own multiplayer is off in this instance\n", MR8(HOOK_SITE), MR8(HOOK_SITE + 1), MR8(HOOK_SITE + 2),
            MR8(HOOK_SITE + 3), MR8(HOOK_SITE + 4), g_thunk);
    g_state = 1;
}

/* the main-loop hook: 1 when the plugin is here (then the Mac build's
 * roam_game_frame does not run) */
int roam_asipeer_frame(CPU *c){
    if (!g_state){ if (!getenv("NFSU2_ROAM_ASI")) { g_state = -1; return 0; } start(c); }
    if (g_state < 0) return getenv("NFSU2_ROAM_ASI") != 0;
    if (MR8(HOOK_SITE) != 0xE8) return 1;
    int32_t rel; memcpy(&rel, g_mem + HOOK_SITE + 1, 4);
    uint32_t tgt = HOOK_SITE + 5u + (uint32_t)rel;
    if (tgt < g_pe.base || tgt >= g_pe.base + g_pe.size) return 1;     /* the plugin did not hook: the game's own call */
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    CPU save = *c;
    g_c = c;
    c->esp -= 4u;                                                      /* the call's return slot */
    uint32_t r[XR__N] = { c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi, 0, eflags_get(c) & ~0x00240000u };
    cpu_to_uc(g_vm, c);
    int ok = xvm_call(g_vm, tgt, r);
    if (!ok){ fprintf(stderr, "[asipeer] f=%ld the plugin's frame stopped: %s -- plugin off\n", autotest_frame(), xvm_why(g_vm)); g_state = -1; }
    else if (r[XR_ESP] != save.esp || r[XR_EAX] != save.eax || r[XR_EBX] != save.ebx || r[XR_ESI] != save.esi || r[XR_EDI] != save.edi || r[XR_EBP] != save.ebp){
        static int warned; if (!warned++) fprintf(stderr, "[asipeer] f=%ld the plugin's thunk came back with other registers\n", autotest_frame()); }
    *c = save;
    g_frames++;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    g_ns += (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000000 + (t1.tv_nsec - t0.tv_nsec));
    return 1;
}
void roam_asipeer_report(void){
    static int done;
    if (g_state == 0 || !g_vm || done++) return;
    fprintf(stderr, "\n  [asipeer] the Windows plugin ran %llu frames under Unicorn (%.1f us per frame with its game calls), "
                    "%llu calls into the game bridged, %llu Win32 calls, %llu bytes sent, %llu received, %llu refused (not loopback), %d thread(s) refused\n",
            (unsigned long long)g_frames, g_frames ? g_ns / 1000.0 / (double)g_frames : 0.0, (unsigned long long)g_bridged,
            (unsigned long long)g_w.calls, (unsigned long long)g_w.bytes_sent, (unsigned long long)g_w.bytes_recv,
            (unsigned long long)g_blocked, g_w.threads_refused);
    if (g_state > 0){
        /* the plugin's DLL_PROCESS_DETACH, as at game exit: its report and BYE */
        uint32_t r[XR__N] = {0}; r[XR_ESP] = 0x7FEF0000u;
        uint32_t a[4] = { 0, g_pe.base, 0, 1 }; memcpy(g_mem + r[XR_ESP], a, 16);
        CPU tmp; memset(&tmp, 0, sizeof tmp); tmp.esp = r[XR_ESP]; tmp.fcw = 0x027f; g_c = &tmp;
        if (!xvm_call(g_vm, g_pe.entry, r)) fprintf(stderr, "  [asipeer] DllMain(PROCESS_DETACH) stopped: %s\n", xvm_why(g_vm));
    }
    aw32_close(&g_w);
}

/* ---- script commands: "roam asi ..." (what the game shows, nothing of the plugin's) ---- */
static int friend_cars(uint32_t *out, int max){
    uint32_t w = mem_is_committed(G_WORLD) ? MR32(G_WORLD) : 0, me = 0, p = MR32(G_PLAYER1);
    if (p && mem_is_committed(p + 4)) me = MR32(p + 4);
    if (!w || !mem_is_committed(w)) return 0;
    int n = 0; uint32_t it = MR32(w);
    for (int k = 0; it && it != w && k < 64 && mem_is_committed(it) && mem_is_committed(it + 0x9e0); k++, it = MR32(it)){
        if (it == me) continue;
        float z = MRF32(it + 0x68u);
        /* inert (no brain, no physics), not in the parking spot: a friend's car */
        if (MR32(it + 0x24u) == 0 && MR32(it + 0x28u) == 0 && z > PARK_Z + 1000.f && n < max) out[n++] = it;
    }
    return n;
}
int roam_asipeer_cmd(CPU *c, const char *args, long *wait_start){
    (void)c;
    char sub[32] = {0}, a1[64] = {0}, a2[64] = {0};
    sscanf(args, "%31s %63s %63s", sub, a1, a2);
    long f = autotest_frame();
    uint32_t cars[8]; int n = friend_cars(cars, 8);
    if (!strcasecmp(sub, "status")){
        fprintf(stderr, "[asipeer] f=%ld status: plugin %s, %llu frames, %d friend car(s) shown\n", f, g_state > 0 ? "running" : "off",
                (unsigned long long)g_frames, n);
        for (int i = 0; i < n; i++){
            uint32_t rec = MR32(cars[i] + 0x14u); char nm[17] = {0};
            if (rec && mem_is_committed(rec + 0x90cu)) for (int k = 0; k < 16 && MR8(rec + 0x90cu + (uint32_t)k) >= 0x20 && MR8(rec + 0x90cu + (uint32_t)k) < 0x7f; k++) nm[k] = (char)MR8(rec + 0x90cu + (uint32_t)k);
            fprintf(stderr, "[asipeer]   car 0x%08x (%s) at %.1f %.1f %.1f, speed %.1f, heading 0x%04x\n", cars[i], nm, (double)MRF32(cars[i] + 0x60u),
                    (double)MRF32(cars[i] + 0x64u), (double)MRF32(cars[i] + 0x68u), (double)MRF32(cars[i] + 0x42cu), MR16(cars[i] + 0x40cu));
        }
        return 1;
    }
    if (!strcasecmp(sub, "waitcars")){
        int want = atoi(a1); long tmo = a2[0] ? atol(a2) : 0;
        if (*wait_start < 0) *wait_start = f;
        if (n >= want){ fprintf(stderr, "[asipeer] f=%ld waitcars %d satisfied after %ld frames\n", f, want, f - *wait_start); *wait_start = -1; return 1; }
        if (tmo && f - *wait_start >= tmo){ fprintf(stderr, "[asipeer] f=%ld waitcars %d TIMEOUT (have %d)\n", f, want, n); *wait_start = -1; return 1; }
        return 0;
    }
    if (!strcasecmp(sub, "follow")){
        int k = atoi(a1); float back = a2[0] ? (float)atof(a2) : 12.f;
        if (k < 0 || k >= n){ fprintf(stderr, "[asipeer] f=%ld follow %d: no friend's car there\n", f, k); return 1; }
        uint32_t car = cars[k];
        float fx = MRF32(car + 0x90u), fy = MRF32(car + 0x94u), lx = MRF32(car + 0xa0u), ly = MRF32(car + 0xa4u), side = 3.5f;
        MRF32(0x00890140u) = MRF32(car + 0x60u) - back * fx + side * lx; MRF32(0x00890144u) = MRF32(car + 0x64u) - back * fy + side * ly;
        MRF32(0x00890148u) = MRF32(car + 0x68u) + 0.5f;
        MR16(0x008900d0u) = MR16(car + 0x40cu); MR8(0x008900ceu) = 1; MR8(0x008900cdu) = 1;
        fprintf(stderr, "[asipeer] f=%ld follow: behind car 0x%08x at %.1f %.1f %.1f\n", f, car, (double)MRF32(car + 0x60u), (double)MRF32(car + 0x64u), (double)MRF32(car + 0x68u));
        return 1;
    }
    fprintf(stderr, "[asipeer] f=%ld asi: unknown '%s'\n", f, sub);
    return 1;
}
