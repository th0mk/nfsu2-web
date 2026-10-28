/* Most Wanted's threads by name, for the stall watchdog's report and the
 * [thread] lines (runtime/game.h thread_names, thread_start; GAME.md 6.2).
 *
 * speed.exe calls CreateThread at eight places, each with its start
 * routine: 0x820C82 (0x820BD7), 0x84AE31 (0x84AC10), 0x84B751 (0x84B5F0),
 * 0x84C075 (0x84BFD0), 0x84E000 (0x84D980), 0x8586BB (0x857810), 0x8649A7
 * (0x8648F0), and 0x85D695 in the C runtime's _beginthreadex (0x85D600),
 * whose start routine is always _threadstartex (0x85D6E0) with the thread's
 * data block as argument: the routine to run at +0x4C, its argument at +0x50.
 * _beginthreadex is called from 0x83572E (0x835670), 0x835843 (0x835780) and
 * from the game's own thread maker 0x7EC44F at 0x7EC486.  That one starts
 * every thread at the trampoline 0x7EC1D3, CREATE_SUSPENDED, with a block on
 * its stack {started flag, routine, argument}: the trampoline copies it, sets
 * the flag and calls the routine, and the maker waits for the flag (SleepEx
 * 1 ms) before it returns, so the block is there while CreateThread runs.
 * Its routines: 0x7EC601 (from 0x7EC6F0), 0x5265C0 (0x5265A9), 0x7976B0
 * (0x799F10), 0x7ED55E (0x7ED7B5), 0x7EF80A (0x7EF8E1).  (0x6DB6C0, called a
 * thread start elsewhere, is the window procedure: WNDCLASSEX.lpfnWndProc,
 * 0x6E6AED.)
 *
 * The names come from what each routine is: the timer thread (GAME.md 6.1)
 * and the sound thread (7.2) are verified; socket, network, modem and voice
 * from the libraries they are in (GAME.md 3.3, 6.2); file and prefetch from
 * the nearest source path EA's RealCore left in their module (filesys.cpp
 * from 0x7ED0ED, prefetch.cpp from 0x7EEFF3); lobby from the Online lobby's
 * tags around them (NAME, KICK, PERSONAS ...): inferred, not seen running. */
#include "cpu.h"
#include "game.h"

const ThreadName mw_thread_names[] = {
    { 0x007c4040u, "main" },                /* the entry point: the main thread (the window and the frames) */
    { 0x007ec601u, "timer" },               /* the 100 Hz waitable timer, [0x9BB130] */
    { 0x00820bd7u, "sound" },               /* polls DirectSound every 10 ms (0x820907) */
    { 0x0084ac10u, "socket" },              /* DirtySock's overlapped receives */
    { 0x0084b5f0u, "network" },
    { 0x0084bfd0u, "network" },
    { 0x0084d980u, "network" },
    { 0x00857810u, "modem" },               /* COM port and TAPI */
    { 0x008648f0u, "voice" },               /* DirtySock's voice chat */
    { 0x007ed55eu, "file" },
    { 0x007ef80au, "prefetch" },
    { 0x007976b0u, "lobby" },
    { 0x00835670u, "lobby" },
    { 0x00835780u, "lobby" },
    { 0x005265c0u, "front end job" },       /* waits for its object's go (+0x24), then calls its +8 once */
    { 0, 0 },
};

static uint32_t rd(uint32_t a, int *ok){
    if (a && mem_is_committed(a) && mem_is_committed(a + 3u)) return MR32(a);
    *ok = 0; return 0;
}
/* CreateThread(entry, arg): the routine the thread really runs */
uint32_t mw_thread_start(uint32_t entry, uint32_t arg){
    if (entry != 0x0085d6e0u) return entry;           /* not _threadstartex: its own routine */
    int ok = 1;
    uint32_t fn = rd(arg + 0x4cu, &ok), a = rd(arg + 0x50u, &ok);
    if (!ok || !fn) return entry;
    if (fn != 0x007ec1d3u) return fn;                 /* _beginthreadex's own routine */
    uint32_t g = rd(a + 4u, &ok);                     /* the game's trampoline: the block's routine */
    return ok && g ? g : fn;
}
