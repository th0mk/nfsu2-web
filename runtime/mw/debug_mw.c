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
 * Numbers take C's prefixes (0x...).  The race aids (racewin, autopilot, teleport, where, racestatus,
 * frames) are career_mw.c's; career_cmd hands them on. */
#include "cpu.h"
#include "game.h"
#include "module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

long autotest_frame(void);
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);

int mw_aid_is_cmd(const char *cmd); int mw_aid_cmd(CPU *c, const char *line, long *wait_start);  /* career_mw.c */

int career_is_cmd(const char *cmd){
    return !strcasecmp(cmd, "call") || !strcasecmp(cmd, "loadlib") || mw_aid_is_cmd(cmd);
}

/* line = "<cmd> <args>"; 1 = done (these two never wait; the race aids of career_mw.c wait for the frame
 * anchor) */
int career_cmd(CPU *c, const char *line, long *wait_start){
    char cmd[32] = {0}; int used = 0;
    if (sscanf(line, "%31s %n", cmd, &used) < 1) return 1;
    if (mw_aid_is_cmd(cmd)) return mw_aid_cmd(c, line, wait_start);
    const char *p = line + used;
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

/* The game's instruction hooks (game.h hook): MW's own one, the frame anchor, is core's (frame_site), so a hit
 * here is a debugging one, an address a lift was given with gen.sh's HOOKS=... (a function's first
 * instruction, say).  NFSMW_HOOKLOG=1 logs the first 20 hits of each address, with the return address and
 * the first three stack arguments at a function's entry: "[hook] f=N 0xADDR from RET args A1 A2 A3 ecx X". */
int mw_debug_hook(CPU *c, uint32_t va){
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
