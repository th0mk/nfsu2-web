/* Most Wanted's test-only script commands (runtime/autotest.c hands it every
 * command it does not know itself; UG2's are in runtime/career.c, a game
 * without its own has runtime/web/null_game.c's none).  Both run on the guest
 * main thread at Present, with the guest lock held, and both are for test
 * runs only: no player's page sends a script.
 *
 *   call ADDR [ecx=X] [ARG ...]   call the guest function at ADDR with the
 *                                 arguments pushed right to left (stdcall or
 *                                 cdecl: the stack is restored either way),
 *                                 ecx for a member function; logs eax
 *   loadlib NAME                  LoadLibraryA(NAME) as the game's own call
 *                                 makes it (module.c: map, relocate, bind,
 *                                 DllMain), then FreeLibrary; a lifted DLL
 *                                 the game loads only later (server.dll for
 *                                 a LAN host) can be checked at boot
 *   movie NAME                    play the movie NAME ("blacklist_01": the file
 *                                 MOVIES\NAME_<language>_<region>.vp6) as the
 *                                 Blacklist rap sheet plays one: the name in
 *                                 0x8F3750, FEAnyMovie.fng brought up; back on
 *                                 the screen under it when the movie ends or is
 *                                 skipped (the game's own rule, 0x559880: input
 *                                 ends it 5 s in, at once for a player on the
 *                                 Blacklist)
 *   movieup [FRAMES]              wait until FEAnyMovie.fng is in the front
 *                                 end's active list (at most FRAMES, default 600;
 *                                 TIMEOUT if not): the screen name of a waitfng
 *                                 can still be the last movie's
 *   movieend [FRAMES]             wait until it has gone (at most FRAMES, default
 *                                 20000; TIMEOUT if not) and log how long that
 *                                 wait took and how long the movie was up, in
 *                                 frames and ms
 *   waitgone PACKAGE [FRAMES]     wait until no package of that name is in the
 *                                 active list (at most FRAMES, default 20000;
 *                                 TIMEOUT if not), logging how long it took
 *   objects                       the kernel objects in use, by kind and the
 *                                 code that made each ("[objects] ..." line,
 *                                 win32_ext.c win32_obj_creators)
 * Numbers take C's prefixes (0x...).  The race aids (racewin, autopilot, teleport, where, racestatus,
 * frames) are career_mw.c's; career_cmd hands them on. */
#include "cpu.h"
#include "game.h"
#include "module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

long autotest_frame(void);
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);

int mw_aid_is_cmd(const char *cmd); int mw_aid_cmd(CPU *c, const char *line, long *wait_start);  /* career_mw.c */

int career_is_cmd(const char *cmd){
    return !strcasecmp(cmd, "call") || !strcasecmp(cmd, "loadlib") || !strcasecmp(cmd, "objects") ||
           !strcasecmp(cmd, "movie") || !strcasecmp(cmd, "movieup") || !strcasecmp(cmd, "movieend") ||
           !strcasecmp(cmd, "waitgone") || mw_aid_is_cmd(cmd);
}
void win32_obj_creators(long frame);           /* win32_ext.c */
int mw_fe_active(const char *name);             /* fe_mw.c */
static double mono_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

/* line = "<cmd> <args>"; 1 = done (these two never wait; the race aids of career_mw.c wait for the frame
 * anchor) */
int career_cmd(CPU *c, const char *line, long *wait_start){
    char cmd[32] = {0}; int used = 0;
    if (sscanf(line, "%31s %n", cmd, &used) < 1) return 1;
    if (mw_aid_is_cmd(cmd)) return mw_aid_cmd(c, line, wait_start);
    const char *p = line + used;
    if (!strcasecmp(cmd, "objects")){ win32_obj_creators(autotest_frame()); return 1; }
    if (!strcasecmp(cmd, "movie")){
        char nm[48] = {0};
        if (sscanf(p, "%47s", nm) != 1){ fprintf(stderr, "[mwtest] movie: no name\n"); return 1; }
        /* as the rap sheet does it (0x5302E9..0x53054C): the name into the front end's movie name 0x8F3750
         * (0x40 bytes), then cFEng [0x91CADC] ->0x525940(package, 0, 0, 0) brings up FEAnyMovie.fng, or
         * WS_FEAnyMovie.fng when 0x4FB000 answers 1 (the widescreen packages) */
        memset(g_mem + 0x008f3750u, 0, 0x40u);
        memcpy(g_mem + 0x008f3750u, nm, strlen(nm));
        uint32_t keep = c->ecx;
        uint32_t ws = call_guest(c, 0x004fb000u, 0, 0) & 0xffu;
        uint32_t arg[4] = { ws ? 0x00897318u : 0x0089732cu, 0, 0, 0 };
        fprintf(stderr, "[mwtest] f=%ld movie %s (%s)\n", autotest_frame(), nm, ws ? "WS_FEAnyMovie.fng" : "FEAnyMovie.fng");
        c->ecx = MR32(0x0091cadcu);
        call_guest(c, 0x00525940u, 4, arg);
        c->ecx = keep;
        return 1;
    }
    if (!strcasecmp(cmd, "waitgone")){
        static long w_f; static double w_t;
        char pkg[64] = {0}; long lim = 0, f = autotest_frame();
        if (sscanf(p, "%63s %ld", pkg, &lim) < 1){ fprintf(stderr, "[mwtest] waitgone: no package\n"); return 1; }
        if (lim <= 0) lim = 20000;
        int on = mw_fe_active(pkg);
        if (*wait_start < 0){ *wait_start = f; w_f = f; w_t = mono_ms(); }
        if (on && f - *wait_start < lim) return 0;
        fprintf(stderr, "[mwtest] f=%ld %s %s after %ld frames, %.0f ms\n", f, pkg, on ? "still up: TIMEOUT" : "gone",
                f - w_f, mono_ms() - w_t);
        *wait_start = -1;
        return 1;
    }
    if (!strcasecmp(cmd, "movieup") || !strcasecmp(cmd, "movieend")){
        static long up_f, w_f; static double up_t, w_t;    /* when movieup saw it, when this wait began */
        int end = !strcasecmp(cmd, "movieend");
        long lim = 0, f = autotest_frame();
        if (sscanf(p, "%ld", &lim) != 1 || lim <= 0) lim = end ? 20000 : 600;
        int on = mw_fe_active("FEAnyMovie.fng") || mw_fe_active("WS_FEAnyMovie.fng");
        if (*wait_start < 0){ *wait_start = f; w_f = f; w_t = mono_ms(); }
        if (on == end && f - *wait_start < lim) return 0;     /* movieup waits while it is not up, movieend while it is */
        double t = mono_ms();
        if (!end){
            up_f = f; up_t = t;
            fprintf(stderr, "[mwtest] f=%ld movie %s after %ld frames, %.0f ms\n", f,
                    on ? "up" : "not up: TIMEOUT", f - w_f, t - w_t);
        } else
            fprintf(stderr, "[mwtest] f=%ld movie %s after %ld frames, %.0f ms; up %ld frames, %.0f ms\n", f,
                    on ? "still up: TIMEOUT" : "gone", f - w_f, t - w_t, f - up_f, t - up_t);
        *wait_start = -1;
        return 1;
    }
    if (!strcasecmp(cmd, "call")){
        uint32_t fn = 0, ecx = 0, arg[15]; int na = 0, have_fn = 0;
        char tok[64];
        while (sscanf(p, "%63s %n", tok, &used) == 1){
            p += used;
            if (!have_fn){ fn = (uint32_t)strtoul(tok, 0, 0); have_fn = 1; }
            else if (!strncasecmp(tok, "ecx=", 4)) ecx = (uint32_t)strtoul(tok + 4, 0, 0);
            else if (na < 15) arg[na++] = (uint32_t)strtoul(tok, 0, 0);
        }
        if (!fn){ fprintf(stderr, "[mwtest] call: no function address\n"); return 1; }
        fprintf(stderr, "[mwtest] f=%ld call 0x%08x(ecx=0x%08x, %d args)\n", autotest_frame(), fn, ecx, na);
        uint32_t keep = c->ecx; c->ecx = ecx;
        uint32_t r = call_guest(c, fn, na, arg);
        c->ecx = keep;
        fprintf(stderr, "[mwtest] f=%ld call 0x%08x -> eax=0x%08x (%d)\n", autotest_frame(), fn, r, (int)r);
        return 1;
    }
    if (!strcasecmp(cmd, "loadlib")){
        char nm[64] = {0};
        if (sscanf(p, "%63s", nm) != 1){ fprintf(stderr, "[mwtest] loadlib: no name\n"); return 1; }
        uint32_t err = 0, h = module_load(c, nm, &err);
        fprintf(stderr, "[mwtest] f=%ld loadlib %s -> 0x%08x (error %u)\n", autotest_frame(), nm, h, err);
        if (h) fprintf(stderr, "[mwtest] f=%ld freelib %s -> %d\n", autotest_frame(), nm, module_free(c, h));
        return 1;
    }
    return 1;
}

/* The game's instruction hooks (game.h hook): the frame anchor is core's (frame_site); the crash fixes' sites
 * (fixes_mw.c HOOK_SITE) go to mw_fix_hook first; any other hit is a debugging one, an address a lift was given
 * with gen.sh's HOOKS=... (a function's first instruction, say).  NFSMW_HOOKLOG=1 logs the first 20 hits of each address, with the return address and
 * the first three stack arguments at a function's entry: "[hook] f=N 0xADDR from RET args A1 A2 A3 ecx X". */
int mw_fix_hook(CPU *c, uint32_t va);          /* fixes_mw.c: the crash fixes' instruction hooks */
int mw_debug_hook(CPU *c, uint32_t va){
    if (mw_fix_hook(c, va)) return 1;
    static int on = -1;
    if (on < 0){ const char *e = getenv("NFSMW_HOOKLOG"); on = e && atoi(e); }
    if (!on) return 0;
    static struct { uint32_t va; int n; } seen[512];
    static int ns;
    int i = 0;
    while (i < ns && seen[i].va != va) i++;
    if (i == ns){ if (ns == 512) return 0; seen[ns].va = va; seen[ns].n = 0; ns++; }
    if (seen[i].n++ >= 20) return 0;
    fprintf(stderr, "[hook] f=%ld 0x%08x from 0x%08x args %08x %08x %08x ecx %08x\n", autotest_frame(), va,
            MR32(c->esp), MR32(c->esp + 4u), MR32(c->esp + 8u), MR32(c->esp + 12u), c->ecx);
    return 1;
}
