/* slow_nx.c -- makes the game thread as slow as the console's, for tests on a faster machine (yuzu on a PC).
 * With the file /switch/nfsu2/slowdown.txt (one number K, e.g. 8; the console's threads take about K times what the PC's do) each frame
 * the game thread spends, after its own work, (K-1) times that work's CPU time again in a busy wait.  Not set on a real console.
 * Calibrate K by comparing the same screen's gcpu in the console's log and in yuzu's (tools: compare lines "[status] ... gcpu=").
 * Called by the game thread once a frame (text_nx.c input_web_frame). */
#include <stdlib.h>
#include <time.h>

static double thread_ms(void){ struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static double wall_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

void nfsu2_slowdown_frame(void){
    static double k = -1, last;
    if (k < 0){ const char *e = getenv("NFSU2_SLOWDOWN"); k = e ? atof(e) : 0; last = thread_ms(); }
    if (k <= 1.0) return;
    double now = thread_ms(), work = now - last;
    if (work > 0 && work < 200){
        double until = wall_ms() + work * (k - 1.0);
        while (wall_ms() < until) { }
    }
    last = thread_ms();                                         /* the wait is not work */
}
