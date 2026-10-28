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
 * Numbers take C's prefixes (0x...). */
#include "cpu.h"
#include "game.h"
#include "module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

long autotest_frame(void);
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);

int career_is_cmd(const char *cmd){
    return !strcasecmp(cmd, "call") || !strcasecmp(cmd, "loadlib");
}

/* line = "<cmd> <args>"; 1 = done (these never wait) */
int career_cmd(CPU *c, const char *line, long *wait_start){
    (void)wait_start;
    char cmd[32] = {0}; int used = 0;
    if (sscanf(line, "%31s %n", cmd, &used) < 1) return 1;
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
