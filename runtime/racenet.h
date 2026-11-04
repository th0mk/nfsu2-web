/* racenet.h -- a network race's links as the browser's virtual LAN
 * (runtime/web/net_lan.c) saw them, for racenet.c's lines at the race's end.
 * The Mac's LAN has none (racenet.c's weak stand-in). */
#ifndef RACENET_H
#define RACENET_H
#include <stdint.h>

/* the datagrams that came from one race peer (the game's UDP 3658) */
typedef struct {
    uint32_t ip_h;          /* its address (host order) */
    uint32_t n;             /* datagrams */
    uint32_t gap_max_ms;    /* the longest wait for one */
    uint32_t gaps;          /* waits of RACENET_GAP_MS or more */
    uint32_t after;         /* datagrams that came at once (RACENET_ONCE_MS apart at most) ending those waits */
    uint32_t runs;          /* runs of datagrams that came at once, those included */
} RaceLink;
#define RACENET_GAP_MS  250u
#define RACENET_ONCE_MS 5u

/* the race links with datagrams since the last reset, at most max of them
 * (their number), then their numbers back to 0 when reset is 1 */
int vlan_race_links(RaceLink *o, int max, int reset);

/* the largest jump of the car of the player named so (its LAN name) in the race just raced, as our game had it: a
 * frame's move that differs from the frame before's, right after a stall of its states less what a car drives in
 * that time (m; 0: none, or no such car).  Kept until the next race begins */
float racenet_jump(const char *name);
/* of the other cars of the race just raced, the one that jumped farthest (racenet_jump): how far (m; 0 none) and its
 * player's LAN name ("" none) */
float racenet_farthest(char name[16]);
#endif
