/* Most Wanted's sound thread and DirectSound (GAME.md 7.2), as the game
 * table hands them to core (runtime/game.h on_sound_position,
 * sound_wall_catchup).
 *
 * The sound thread (0x820BD7) runs SND's service 0x820907 about every 10 ms:
 * GetStatus, then GetCurrentPosition for the write cursor.  When the cursor is
 * where it was last time ([0x9C22A0]) the service counts [0x9C229C] up, and at
 * 100 (`cmp [0x9C229C], 100` at 0x82097A) it releases DirectSound and its
 * buffers (0x8201AE) and creates them again (0x820762) while the engine still
 * uses the old ones: on Windows a stalled sound device crashes the game about
 * two seconds later.  Any move of the cursor sets the count back to 0
 * (0x8209AA).  So the cursor must move in real time, always: the mixer
 * (runtime/web/audio_web.c) keeps it on the wall clock whenever no
 * AudioContext plays (no click on the page yet, a hidden tab, a stalled
 * device), and the game table asks it to make up every backlog there
 * (sound_wall_catchup).
 *
 * This watches that it does.  The service reads the cursor right after the
 * count was last updated, so the count at each GetCurrentPosition is every
 * value it reaches: each new highest one is logged ("[mw-sound] ..."; it is 1
 * or 2 while the cursor moves every 11.6 ms slice), and a count that reaches
 * the limit is logged as the rebuild it starts. */
#include "cpu.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MW_SND_STILL  0x009c229cu      /* the service's polls with a still write cursor */
#define MW_SND_LIMIT  100u             /* ... at which it rebuilds DirectSound (0x82097A) */

static uint32_t g_still_max = 0;
static unsigned g_rebuilds = 0;
static int      g_at_limit = 0;

void mw_sound_position(void){
    uint32_t n = MR32(MW_SND_STILL);
    if (n > g_still_max && n <= 1000000u){
        g_still_max = n;
        fprintf(stderr, "[mw-sound] the sound thread saw a still DirectSound cursor %u polls in a row (its limit: %u)\n",
                n, MW_SND_LIMIT);
    }
    if (n >= MW_SND_LIMIT && !g_at_limit)
        fprintf(stderr, "[mw-sound] DirectSound STALL: %u polls without a cursor move, the game rebuilds DirectSound "
                "under its engine (0x8201AE; %u so far)\n", n, ++g_rebuilds);
    g_at_limit = n >= MW_SND_LIMIT;
}

/* Tunnels (TRACKER M4-27, test aid).  A car's sound object (this = esi) looks up the world's sound zones at the
 * car's place every update (0x4B5620(place, type), the place [[this+0x14]+0x34]+0x40): a zone of type 3, 4, 5
 * (tunnels), 7 or 10 (with +0x1A == 1) puts the car "inside" (the byte this+0x28 set at 0x4C5D4B, the type kept at
 * this+0x38), any other place out again (cleared at 0x4C5CC7).  On each change, unless [[this+0x14]+0x34]+0x228 is
 * set, the game posts its "TunnelUpdate" message and switches the reverb (0x4B57E0) to a preset of the table at
 * 0x8EFCC0: type 3 Simple_Tunnel (5), 4 and 5 Simple_Tunnel_Sml (6), 7 and 10 Garage (3); out again the area's own
 * ([0x8EFC90 + 4 * this+0x74]).  With NFSMW_TUNNEL_LOG=1 each change is logged:
 *   "[mw-sound] f=N tunnel in: zone type T at X Y Z (car sound object O)" and "... tunnel out ...". */
#define HOOK_SITE(va) (va##u)
#define SITE_TUNNEL_IN   HOOK_SITE(0x004c5d4b)   /* mov byte [esi+0x28], 1 */
#define SITE_TUNNEL_OUT  HOOK_SITE(0x004c5cc7)   /* mov byte [esi+0x28], 0 */
long autotest_frame(void);
int mw_sound_hook(CPU *c, uint32_t va){
    if (va != SITE_TUNNEL_IN && va != SITE_TUNNEL_OUT) return 0;
    static int on = -1;
    if (on < 0){ const char *e = getenv("NFSMW_TUNNEL_LOG"); on = e && *e && *e != '0'; }
    if (!on) return 1;
    uint32_t o = c->esi, s = mem_is_committed(o + 0x14u) ? MR32(o + 0x14u) : 0u;
    uint32_t car = s && mem_is_committed(s + 0x34u) ? MR32(s + 0x34u) : 0u;
    float p[3] = { 0, 0, 0 };
    uint32_t quiet = 0;
    if (car && mem_is_committed(car + 0x22Bu)){
        for (int i = 0; i < 3; i++){ uint32_t u = MR32(car + 0x40u + 4u * (uint32_t)i); memcpy(&p[i], &u, 4); }
        quiet = MR32(car + 0x228u);
    }
    fprintf(stderr, "[mw-sound] f=%ld tunnel %s: zone type %u at %.1f %.1f %.1f (car sound object %08x%s)\n",
            autotest_frame(), va == SITE_TUNNEL_IN ? "in" : "out",
            mem_is_committed(o + 0x3Bu) ? MR32(o + 0x38u) : 0u, p[0], p[1], p[2], o, quiet ? ", no reverb change" : "");
    return 1;
}
