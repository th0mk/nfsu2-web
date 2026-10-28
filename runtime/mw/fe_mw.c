/* Most Wanted's game flow and front end, as core's logs and the test
 * scripts read them (runtime/game.h flow, top_screen, on_test_frame;
 * runtime/autotest.c waitmem, waitfng, tapuntil and the [fng] lines).  It
 * only reads.  The addresses are speed.exe v1.3's, from its code (GAME.md
 * 13.1, 13.2).
 *
 * GameFlowManager's state is the dword 0x925E90, numbered as UG2's: 1
 * loading the front end, 2 unloading it, 3 in the front end, 5 loading a
 * track, 6 racing, 7 unloading the track, 8 unloading the region, 9 leaving.
 * Its writers: 0x664B80 (1), 0x667010 (2), 0x64B6D0 (3), 0x666FA0 (5),
 * 0x6627E0 (6), 0x6617E0 (7), 0x664250 (8), 0x6597F0 (9); 4 comes through a
 * register.
 *
 * The front end: cFEng is [0x91CADC] and its FEngine [cFEng].  FEngine+0xD8
 * is the list of active packages (count +0xDC, head +0xE0, tail +0xE4) and
 * +0xE8 the idle ones (head +0xF0): FindPackage looks a name up in the first,
 * then the second (0x5B5B70 and 0x5B7580, both 0x5B5B10), and UnloadPackage
 * (0x5BE6A0) moves a package from the first to the second.  A package's next
 * is at +4, its previous at +8, its name at +0xC and the name's hash at
 * +0x10.  FEngine's render (0x5BE800) keeps the active list sorted by the
 * packages' priority (+0x1C) and draws it from head to tail, so the tail is
 * the package on top, the top screen (UG2's GetTopPackage 0x50C8E0 reads
 * its FEngine's +0xE4 too).
 *
 * The screens the game brings up: UG2 has them from its debug printf ("Init
 * Package %s", "SwitchFEngPackages from %s to %s", overrides.c), but MW's
 * retail exe has neither string.  So at every Present (on_test_frame) the
 * active list is walked and each package that was not in it the frame before
 * is noted (autotest_note_screen), which gives MW the same screen lines,
 * waitfng and tapuntil matches. */
#include "cpu.h"
#include <string.h>
#include <strings.h>

#define G_FLOW     0x00925e90u      /* GameFlowManager's state */
#define G_CFENG    0x0091cadcu      /* cFEng* */
#define ENG_HEAD   0xe0u            /* FEngine: the active packages' head */
#define ENG_TAIL   0xe4u            /* and tail: the package on top */
#define PKG_NEXT   0x04u
#define PKG_NAME   0x0cu
#define PKG_HASH   0x10u

static uint32_t rd(uint32_t a){ return a && mem_is_committed(a) && mem_is_committed(a + 3u) ? MR32(a) : 0u; }

uint32_t mw_flow(void){ return rd(G_FLOW); }

/* a package's name, if it reads as one ("MainMenu.fng"), else 0 */
static const char *pkg_name(uint32_t pkg){
    uint32_t p = rd(pkg + PKG_NAME);
    if (!p || !mem_is_committed(p) || !mem_is_committed(p + 63u)) return 0;
    const char *s = (const char *)(g_mem + p); int n = 0;
    while (n < 63 && s[n] >= 0x20 && s[n] < 0x7f) n++;
    if (s[n] != 0 || n < 5 || strncasecmp(s + n - 4, ".fng", 4)) return 0;
    return s;
}
static uint32_t engine(void){ return rd(rd(G_CFENG)); }

const char *mw_top_screen(void){
    uint32_t e = engine();
    return e ? pkg_name(rd(e + ENG_TAIL)) : 0;
}

/* every Present: the packages new in the active list, head to tail */
void autotest_note_screen(const char *nm);
#define LIST_MAX 32                 /* the walk's bound: the front end has a dozen at most */
void mw_fe_frame(void){
    static struct { uint32_t pkg, hash; } seen[LIST_MAX];
    static int nseen;
    uint32_t pk[LIST_MAX], hs[LIST_MAX]; int n = 0;
    uint32_t e = engine();
    for (uint32_t p = e ? rd(e + ENG_HEAD) : 0u; p && n < LIST_MAX; p = rd(p + PKG_NEXT)){
        pk[n] = p; hs[n] = rd(p + PKG_HASH);
        int old = 0;
        for (int i = 0; i < nseen && !old; i++) old = seen[i].pkg == p && seen[i].hash == hs[n];
        const char *nm = old ? 0 : pkg_name(p);
        if (nm) autotest_note_screen(nm);
        n++;
    }
    for (int i = 0; i < n; i++){ seen[i].pkg = pk[i]; seen[i].hash = hs[i]; }
    nseen = n;
}
