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
