/* cc -Wall -Wextra -Werror tests/opentunnel_unit.c -o out/opentunnel_unit
 * Run each mode in a fresh process: unset, off, on, incomplete, invalid. */
#include <assert.h>
#include "../runtime/opentunnel.h"
static unsigned probes;
static int missing;
int fileio_resolve_game(const char *name, char *out, size_t cap){
    ++probes;
    if (missing && strstr(name, "PathsFreeRoam")) return 0;
    snprintf(out, cap, "/data/%s", name);
    return 1;
}
int main(int argc, char **argv){
    assert(argc == 2);
    unsetenv("NFSU2_OPEN_TUNNEL");
    int on = !strcmp(argv[1], "on");
    missing = !strcmp(argv[1], "incomplete");
    if (on || missing) setenv("NFSU2_OPEN_TUNNEL", "1", 1);
    else if (strcmp(argv[1], "unset")) setenv("NFSU2_OPEN_TUNNEL", !strcmp(argv[1], "off") ? "0" : "garbage", 1);
    char out[1024];
    const char *paths[] = {"tracks/l4ra.bun", "TRACKS/STREAML4RA.BUN", "TRACKS/ROUTESL4RA/PathsFreeRoam.bin"};
    for (unsigned i = 0; i < 3; ++i){
        assert(opentunnel_resolve(paths[i], out, sizeof out) == on);
        if (on) assert(strstr(out, "/MODPACKS/open-tunnel/TRACKS/"));
    }
    assert(!opentunnel_resolve("TRACKS/L4RD.BUN", out, sizeof out));
    assert(!opentunnel_resolve("GLOBAL/GlobalB.lzc", out, sizeof out));
    assert(!opentunnel_resolve(paths[0], out, 2));
    assert(probes == (on || missing ? 3u : 0u));
    puts("PASS tunnel selection");
}
