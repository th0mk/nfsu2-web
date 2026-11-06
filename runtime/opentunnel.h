/* Opt-in Construction Tunnel assets. The original TRACKS stay untouched.
 * Resolve the complete pack once per process: missing files disable it as a
 * whole, so the stream directory, geometry and navigation always agree. */
#ifndef NFSU2_OPENTUNNEL_H
#define NFSU2_OPENTUNNEL_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
int fileio_resolve_game(const char *, char *, size_t);
static int opentunnel_resolve(const char *rel, char *out, size_t cap){
    static const char *const files[] = {
        "TRACKS/L4RA.BUN", "TRACKS/STREAML4RA.BUN", "TRACKS/ROUTESL4RA/PathsFreeRoam.bin"
    };
    static int checked, enabled;
    static char paths[3][1024];
    if (!checked){
        checked = 1;
        const char *v = getenv("NFSU2_OPEN_TUNNEL");
        enabled = v && (!strcmp(v, "1") || !strcasecmp(v, "on") || !strcasecmp(v, "true"));
        if (enabled){
            for (unsigned i = 0; i < 3; ++i){
                char name[256];
                snprintf(name, sizeof name, "MODPACKS/open-tunnel/%s", files[i]);
                if (!fileio_resolve_game(name, paths[i], sizeof paths[i])){
                    enabled = 0;
                    fprintf(stderr, "[open-tunnel] incomplete pack: using the original map\n");
                    break;
                }
            }
            if (enabled) fprintf(stderr, "[open-tunnel] enabled\n");
        }
    }
    if (!enabled) return 0;
    for (unsigned i = 0; i < 3; ++i) if (!strcasecmp(rel, files[i])){
        if (strlen(paths[i]) >= cap) return 0;
        strcpy(out, paths[i]);
        return 1;
    }
    return 0;
}
#endif
