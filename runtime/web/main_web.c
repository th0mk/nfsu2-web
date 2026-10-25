/* The browser entry point: tests/boot.c's main() for the wasm build.
 *
 * Same order as on the Mac: memory, the PE image, TEB/PEB, file roots,
 * dispatch, win32, the game's installs (runtime/game.h), autotest; then the
 * guest's entry point on its own thread.  The page (web/src/host) runs this
 * _start on a game worker and gives every guest thread a worker of its own.
 * There is no window loop here: the renderer, audio and input are driven from
 * the page through shared memory, so this thread just waits for the guest to
 * finish.
 *
 * Paths are the host's mounts: /data is the install folder (read-only),
 * /save the save overlay (saves, registry, settings, logs). */
#include "cpu.h"
#include "game.h"
#include "memmap.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <setjmp.h>
#include <pthread.h>

extern jmp_buf g_exit_jmp; extern int g_exit_code, g_exit_armed; extern int g_bt_on;
void dispatch_init(void); void dispatch_report(void);
void win32_init(void); void win32_report(void);
void com_report(void); void com_set_trace(int);
void fileio_set_root(const char*);
void fileio_set_write_root(const char*);
void fileio_set_readonly_root(int);
void lifecycle_guest_exited(int code); int lifecycle_drain_writes(double max_s);
int win32_freeze_guest(int max_ms);
void guest_lock(CPU*); void guest_unlock(void);
void watch_arm(uint32_t); void watch_dump(const char*,int);
int mem_load_pe(const char *path);
void win32_set_trace_max(uint32_t);
extern uint64_t g_hook_hits, g_gs_fail;
extern void (*g_report_fn)(void);

/* Guest layout on wasm (runtime/memmap.h): the main stack sits below the
 * thread stacks and the heap instead of at 0x7F000000. */
#define TEB_BASE   MM_TEB_MAIN
#define PEB_BASE   MM_PEB
#define STACK_LO   MM_STACK_LO
#define STACK_HI   MM_STACK_HI

static CPU cpu; static CPU *const c = &cpu;

static void all_reports(void){
    static int done; if (done) return; done = 1;
    if (getenv("WATCH_ADDR")) watch_dump("end of run / crash", 24);
    if (g_game.crtheap_va)                  /* the game's C runtime heap mode (game.h) */
        fprintf(stderr, "\n  CRT heap: _crtheap=0x%08x __active_heap=%u\n", MR32(g_game.crtheap_va), MR32(g_game.active_heap_va));
    com_report(); win32_report(); { void vclock_report(void); vclock_report(); }
    { void metal_budget_report(void); metal_budget_report(); }
    { void hfr_report(void); hfr_report(); }
    { void pacer_sleep_report(void); pacer_sleep_report(); }
    { void roam_game_report(void); roam_game_report(); }
    { void gcpu_report(void); gcpu_report(); }
    { void stackprobe_report(void); stackprobe_report(); }
    dispatch_report();
}

static void *guest_main_thread(void *unused){
    (void)unused;
    if (setjmp(g_exit_jmp) == 0){
        g_exit_armed = 1;
        guest_lock(&cpu);
        PUSH32(0xDEADC0DEu);
        g_game.entry(&cpu);                 /* the exe's entry point (game.h) */
        guest_unlock();
        fprintf(stderr, "\n*** guest entry point returned (eip=0x%08x, eax=0x%08x)\n", cpu.eip, cpu.eax);
    } else {
        fprintf(stderr, "\n*** guest exited with code %d\n", g_exit_code);
        guest_unlock();          /* ExitProcess longjmp'd out of a call holding it */
    }
    lifecycle_guest_exited(g_exit_code);
    return 0;
}

int main(int argc, char **argv){
    char dflt[300]; snprintf(dflt, sizeof dflt, "/data/%s", g_game.exe_name);
    const char *exe = argc > 1 ? argv[1] : dflt;
    { extern void autotest_early(void); autotest_early(); }   /* NFSU2_LOG, NFSU2_TIMEOUT */

    mem_init();
    if (mem_load_pe(exe)) return 1;
    mem_commit(TEB_BASE, 0x20000);
    mem_commit(STACK_LO, STACK_HI - STACK_LO);
    fileio_set_root(exe);
    { const char *sd = getenv("NFSU2_SAVEDIR"); if (sd && *sd) fileio_set_write_root(sd); }
    { const char *ro = getenv("NFSU2_READONLY_DATA");
      if (ro && *ro && *ro != '0'){ fileio_set_readonly_root(1);
          fprintf(stderr, "file: game folder is read-only (NFSU2_READONLY_DATA)\n"); } }
    dispatch_init(); win32_init();
    fprintf(stderr, "[game] %s: %s in %s\n", g_game.id, g_game.exe_name, g_game.vroot);
    if (g_game.install) g_game.install();   /* the game's patches and features (game_ug2.c) */
    { extern void autotest_init(void); autotest_init(); }       /* NFSU2_INPUT_SCRIPT ... */
    { extern void status_web_start(void); status_web_start(); }  /* counters for the page overlay */
    { extern void input_web_init(void); input_web_init(); }     /* the page's keys, mouse, pad, focus */
    if (getenv("BT")) g_bt_on = 1;
    { int stackprobe_on(void); stackprobe_on(); }   /* NFSU2_STACKPROBE (runtime/web/stackprobe.c) */
    g_report_fn = all_reports;
    com_set_trace(getenv("COMTRACE") ? 1 : 0);
    if (getenv("TRACE_MAX")) win32_set_trace_max((uint32_t)atoi(getenv("TRACE_MAX")));
    else if (getenv("VERBOSE")) win32_set_trace_max(400u);
    { const char *w = getenv("WATCH_ADDR"); if (w) watch_arm((uint32_t)strtoul(w, 0, 0)); }

    /* TEB (fs:) */
    MR32(TEB_BASE+0x00) = 0xFFFFFFFFu;      /* end of the SEH chain */
    MR32(TEB_BASE+0x04) = STACK_HI;         /* StackBase  */
    MR32(TEB_BASE+0x08) = STACK_LO;         /* StackLimit */
    MR32(TEB_BASE+0x18) = TEB_BASE;         /* Self       */
    MR32(TEB_BASE+0x20) = 100;              /* ProcessId  */
    MR32(TEB_BASE+0x24) = 1;                /* ThreadId   */
    MR32(TEB_BASE+0x2C) = TEB_BASE+0x1000;  /* TLS array  */
    MR32(TEB_BASE+0x30) = PEB_BASE;         /* PEB        */
    mem_commit(PEB_BASE, 0x1000);
    MR32(PEB_BASE+0x08) = 0x00400000u;      /* ImageBaseAddress */
    MR32(PEB_BASE+0xA4) = 5; MR32(PEB_BASE+0xA8) = 1;

    { extern CPU *g_cur_cpu; g_cur_cpu = &cpu; }
    memset(&cpu, 0, sizeof cpu);
    cpu.esp = STACK_HI - 0x1000u;
    cpu.top = 0; cpu.fcw = 0x027f;   /* Windows default: PC=53, RC near, masked */
    cpu.fs_base = TEB_BASE;
    cpu.gs_base = TEB_BASE;
    { void win32_thread_test(CPU *); win32_thread_test(&cpu); }   /* NFSU2_THREAD_TEST (win32_ext.c) */

    if (!g_game.entry){ fprintf(stderr, "no game in this build: nothing to run\n"); return 1; }
    fprintf(stderr, "\n=== running the entry point: 0x%08x ===\n", g_game.entry_va);
    fprintf(stderr, "    TEB=0x%08x  stack=0x%08x..0x%08x  esp=0x%08x\n\n", TEB_BASE, STACK_LO, STACK_HI, cpu.esp);
    {
        pthread_attr_t at; pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 16u * 1024u * 1024u);   /* shadow stack only; see win32_ext.c */
        pthread_t th;
        if (pthread_create(&th, &at, guest_main_thread, 0) != 0){
            fprintf(stderr, "could not start the guest main thread\n"); return 1; }
        pthread_join(th, 0);
    }
    lifecycle_drain_writes(5.0);
    if (!win32_freeze_guest(2000)) fprintf(stderr, "app: a guest thread still holds the guest lock\n");
    fprintf(stderr, "\n  hook hits: %llu   /GS failures: %llu\n",
            (unsigned long long)g_hook_hits, (unsigned long long)g_gs_fail);
    if (getenv("WATCH_ADDR")) watch_dump("end of run", 20);
    all_reports();
    return g_exit_code;
}
