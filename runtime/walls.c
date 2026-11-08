/* walls.c -- a car past the career's walls goes back to the roads it can reach.
 *
 * Career free roam closes the later stages' areas with red walls: the scenery
 * and collision group BARRIERS_CAREER<stage> (0x578060, 0x5F54A0), and lines
 * of the same group among the track's barriers (track 0x88F298 +0x498 count,
 * +0x49C 0x24 each: x0 y0 x1 y1, +0x10 enabled), which the GPS's road search
 * does not cross (0x5D2E60).  In a stage 2 career five such lines are on:
 * where the roads leave its area.  A car thrown past a wall (a hard hit with
 * another player's car in free roam, Discord 2026-11-04: Coal Harbor's bridge;
 * the walls stop a car thrown at them even 40 m up, so round a wall's end or
 * through it) could not come back: R (Player::ResetCar 0x6009A0, also the game's own reset
 * of a car that fell out of the world: 0x6012B0) puts it in a reset zone, on
 * the road it is on (the car's road, +0x2C) or where it was a moment ago; its
 * only look at the walls is the line from the car to that road.  Leaving free
 * roam the game keeps where the car is (Career +0x8750, 0x513B30; an outrun's
 * end 0x513950) and the profile saves that, so every load, garage and race put
 * the car behind the wall again.
 *
 * The traffic's roads (road table 0x88A334, 2048: a road's own index at +0xA,
 * its node count at +0x34, its nodes from +0x8C, 0x38 bytes: x, y, the height
 * at +0x12 (s16, 1/8 m), the road's index in +0xC's low 11 bits; joints
 * 0x883DB0 (count 0x883DB4), 0x20 bytes: the two roads at +8/+0xA, their
 * nodes at +4/+6) make one network over the whole city.  Cut where an enabled
 * barrier line crosses it, the part the new career's start still reaches
 * (the airport's car park, in every career's area) is the career's roads;
 * what else is on the network is past its walls (tests/scripts/walls_*.txt).
 * A car whose nearest road (a height difference past HEIGHT_SLACK counts
 * twice, for roads over roads) is past the walls, and no near one of the
 * career's roads at its height is on its side of them, goes to the nearest
 * place on the career's roads at least WALL_GAP from a barrier line, facing
 * away from where it was, through the hot-position jump (as roam_game.c's
 * car_rescue):
 *  - when career free roam starts with the car there (a profile saved so: the
 *    kept spot follows at the next trip out of free roam),
 *  - after a reset (R, or the game's own: the reset's blink start, car +0x974,
 *    is new).
 * Until then a car past a wall stays where it is.  Everywhere else the game's
 * own reset is left as it is.  Career free roam only (an outrun or a race has
 * barriers of its own), not in a new career's prologue (no car of its own
 * yet); free-roam multiplayer on or off.  Log lines start "[walls]".  Script
 * command (tests): walls [at X Y Z | bars | sweep | fling VX VY VZ]. */
#include "cpu.h"
#include "guest.h"
#include "game.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

long autotest_frame(void);
int mprace_car_ok(void);              /* mprace.c: the career has a car of its own (not a new one's prologue) */
void roam_game_jumped(void);          /* roam_game.c: friends' cars are ghosts until they are clear of ours */

#define G_PLAYER1       SPLIT_PLAYERS_INDEX   /* Player*: +4 its car */
#define G_TRACK         0x0088f298u
#define TK_NBAR         0x498u
#define TK_BARS         0x49cu
#define BAR_SZ          0x24u
#define BAR_ON          0x10u
#define G_ROADS         0x0088a334u
#define NROADS          2048u
#define RD_ID           0x0au         /* s16: the road's own index */
#define RD_NODES        0x34u         /* s16 */
#define RD_NODE0        0x8cu
#define NODE_SZ         0x38u
#define ND_Z            0x12u         /* s16, 1/8 m */
#define G_JOINTS        0x00883db0u
#define G_NJOINTS       0x00883db4u
#define JOINT_SZ        0x20u
#define CAR_RB          0x3cu         /* the car's body: [+0x68] its rigid body while +0x5C */
#define CAR_PLACE       0x60u
#define CAR_FWD         0x90u
#define CAR_HEADING     0x40cu
#define CAR_RESET_T     0x974u        /* the reset's blink start (game ticks): each reset a new one */
#define START_X         1695.25f      /* the new career's start (carspot.c) */
#define START_Y         (-883.64f)
#define MAX_BARS        256
#define WALL_GAP        10.f          /* m: the car goes this far from a barrier line at least */
#define HEIGHT_SLACK    4.f           /* m: height differences past this count twice (roads over roads) */
#define NEAR_MORE       20.f          /* m: a career road this much farther than the road past the walls is not "near" */
#define ENTRY_FRAMES    30            /* free roam's first frames: the car's place then is where the load put it */
#define JUMP_PAUSE      120           /* frames after a jump of ours before the next */
#define PLACE_MAX       100000.f

enum { W_UNKNOWN, W_OPEN, W_PAST };   /* reach[]: not on the start's network, the career's roads, past its walls */
static const char *const STATE[] = { "off the network", "on the career's roads", "past the career's walls" };

static struct {
    int built, ok, n, nseg, nbar, nreach, npast;
    float *x, *y, *z;
    uint8_t *reach;
    int *sa, *sb; uint8_t *scut;
    float bar[MAX_BARS][4];
    uint32_t sig;
} W;

static struct {
    int in, entry_done;   /* in: 1 this stay in free roam is looked at, 2 not (a new career's prologue) */
    long since, jumped_at;
    uint32_t stamp;
    unsigned jumps;
} S;

static uint32_t player_car(void){ uint32_t p = guest_rd32(G_PLAYER1), car = p ? guest_rd32(p + 4u) : 0; return guest_ok(car, CAR_RESET_T + 4u) ? car : 0; }
static uint32_t road(uint32_t i){
    uint32_t r = guest_rd32(G_ROADS + 4u * i);
    if (!guest_ok(r, RD_NODE0) || (int16_t)MR16(r + RD_ID) != (int)i) return 0;   /* the table's other slots name roads again */
    int n = (int16_t)MR16(r + RD_NODES);
    return n > 0 && n < 4096 && guest_ok(r + RD_NODE0, (uint32_t)n * NODE_SZ) ? r : 0;
}
static uint32_t mix(uint32_t h, uint32_t v){ return (h ^ v) * 16777619u; }

/* the roads and the enabled barriers as they are now, in a word */
static uint32_t signature(void){
    uint32_t h = 2166136261u, nb = guest_rd32(G_TRACK + TK_NBAR), bars = guest_rd32(G_TRACK + TK_BARS);
    for (uint32_t i = 0; i < NROADS; i++) h = mix(h, guest_rd32(G_ROADS + 4u * i));
    h = mix(h, guest_rd32(G_JOINTS)); h = mix(h, guest_rd32(G_NJOINTS)); h = mix(h, nb);
    for (uint32_t i = 0; i < nb && i < 8192u && guest_ok(bars + i * BAR_SZ, BAR_SZ); i++) if (MR8(bars + i * BAR_SZ + BAR_ON)) h = mix(h, i + 1u);
    return h;
}

/* 2D: the segments a-b and c-d cross */
static int crosses(float ax, float ay, float bx, float by, const float *c){
    float d1 = (c[2] - c[0]) * (ay - c[1]) - (c[3] - c[1]) * (ax - c[0]);
    float d2 = (c[2] - c[0]) * (by - c[1]) - (c[3] - c[1]) * (bx - c[0]);
    float d3 = (bx - ax) * (c[1] - ay) - (by - ay) * (c[0] - ax);
    float d4 = (bx - ax) * (c[3] - ay) - (by - ay) * (c[2] - ax);
    return (d1 > 0.f) != (d2 > 0.f) && (d3 > 0.f) != (d4 > 0.f);
}
static int crosses_any(float ax, float ay, float bx, float by){
    for (int i = 0; i < W.nbar; i++) if (crosses(ax, ay, bx, by, W.bar[i])) return i + 1;
    return 0;
}
/* 2D distance from (px, py) to the segment a-b, and where along it (0..1) */
static float seg_dist(float px, float py, float ax, float ay, float bx, float by, float *t){
    float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy, u = l2 > 1e-6f ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0.f;
    if (u < 0.f) u = 0.f; else if (u > 1.f) u = 1.f;
    if (t) *t = u;
    float ex = ax + u * dx - px, ey = ay + u * dy - py;
    return sqrtf(ex * ex + ey * ey);
}
static float bar_dist(float x, float y){
    float m = 1e30f;
    for (int i = 0; i < W.nbar; i++){ float d = seg_dist(x, y, W.bar[i][0], W.bar[i][1], W.bar[i][2], W.bar[i][3], 0); if (d < m) m = d; }
    return m;
}
/* how far a place is from a point of the roads: the height difference past HEIGHT_SLACK twice */
static float metric(float d2d, float dz){
    float h = fabsf(dz) - HEIGHT_SLACK;
    if (h < 0.f) h = 0.f;
    return sqrtf(d2d * d2d + 4.f * h * h);
}

static void drop(void){
    free(W.x); free(W.y); free(W.z); free(W.reach); free(W.sa); free(W.sb); free(W.scut);
    W.x = W.y = W.z = 0; W.reach = 0; W.sa = W.sb = 0; W.scut = 0;
    W.built = W.ok = W.n = W.nseg = W.nbar = W.nreach = W.npast = 0;
}

/* the network, its cuts and what the start reaches; W.ok when it is usable */
static void build(long f){
    drop();
    W.built = 1; W.sig = signature();
    uint32_t nb = guest_rd32(G_TRACK + TK_NBAR), bars = guest_rd32(G_TRACK + TK_BARS);
    for (uint32_t i = 0; i < nb && i < 8192u && guest_ok(bars + i * BAR_SZ, BAR_SZ); i++){
        uint32_t b = bars + i * BAR_SZ;
        if (!MR8(b + BAR_ON)) continue;
        if (W.nbar == MAX_BARS){ fprintf(stderr, "[walls] f=%ld more than %d barrier lines on: left as the game has it\n", f, MAX_BARS); return; }
        for (int k = 0; k < 4; k++) W.bar[W.nbar][k] = MRF32(b + 4u * (uint32_t)k);
        if (!isfinite(W.bar[W.nbar][0] + W.bar[W.nbar][1] + W.bar[W.nbar][2] + W.bar[W.nbar][3])) continue;
        W.nbar++;
    }
    static int base[NROADS];
    int n = 0;
    for (uint32_t i = 0; i < NROADS; i++){ uint32_t r = road(i); base[i] = r ? n : -1; if (r) n += (int16_t)MR16(r + RD_NODES); }
    uint32_t nj = guest_rd32(G_NJOINTS), jt = guest_rd32(G_JOINTS);
    if (nj > 65536u || !guest_ok(jt, nj * JOINT_SZ)) nj = 0;
    if (n < 100){ fprintf(stderr, "[walls] f=%ld no roads (%d nodes): left as the game has it\n", f, n); return; }
    int maxseg = n + (int)nj;
    W.x = malloc(sizeof(float) * (size_t)n); W.y = malloc(sizeof(float) * (size_t)n); W.z = malloc(sizeof(float) * (size_t)n);
    W.reach = calloc((size_t)n, 1); W.sa = malloc(sizeof(int) * (size_t)maxseg); W.sb = malloc(sizeof(int) * (size_t)maxseg);
    W.scut = calloc((size_t)maxseg, 1);
    int *deg = calloc((size_t)n + 1, sizeof(int)), *adj = malloc(sizeof(int) * 2u * (size_t)maxseg), *queue = malloc(sizeof(int) * (size_t)n);
    uint8_t *seen = calloc((size_t)n, 1);
    if (!W.x || !W.y || !W.z || !W.reach || !W.sa || !W.sb || !W.scut || !deg || !adj || !queue || !seen){
        fprintf(stderr, "[walls] f=%ld out of memory\n", f); free(deg); free(adj); free(queue); free(seen); drop(); W.built = 1; return;
    }
    W.n = n;
    for (uint32_t i = 0; i < NROADS; i++){
        if (base[i] < 0) continue;
        uint32_t r = road(i); int m = (int16_t)MR16(r + RD_NODES);
        for (int k = 0; k < m; k++){
            uint32_t nd = r + RD_NODE0 + NODE_SZ * (uint32_t)k; int g = base[i] + k;
            W.x[g] = MRF32(nd); W.y[g] = MRF32(nd + 4u); W.z[g] = (float)(int16_t)MR16(nd + ND_Z) * 0.125f;
            if (k + 1 < m){ W.sa[W.nseg] = g; W.sb[W.nseg] = g + 1; W.nseg++; }
        }
    }
    int njoint = 0;
    for (uint32_t j = 0; j < nj; j++){
        uint32_t e = jt + JOINT_SZ * j;
        int n4 = (int16_t)MR16(e + 4u), n6 = (int16_t)MR16(e + 6u), r8 = (int16_t)MR16(e + 8u), ra = (int16_t)MR16(e + 0xau);
        if (r8 < 0 || ra < 0 || r8 >= (int)NROADS || ra >= (int)NROADS || base[r8] < 0 || base[ra] < 0 || n4 < 0 || n6 < 0) continue;
        if (n4 >= (int16_t)MR16(road((uint32_t)r8) + RD_NODES) || n6 >= (int16_t)MR16(road((uint32_t)ra) + RD_NODES)) continue;
        W.sa[W.nseg] = base[r8] + n4; W.sb[W.nseg] = base[ra] + n6; W.nseg++; njoint++;
    }
    int ncut = 0;
    for (int s = 0; s < W.nseg; s++){
        int a = W.sa[s], b = W.sb[s];
        if (isfinite(W.x[a] + W.y[a] + W.x[b] + W.y[b]) && crosses_any(W.x[a], W.y[a], W.x[b], W.y[b])){ W.scut[s] = 1; ncut++; }
    }
    /* the start's node, then everything joined to it (seen), and what of that joins it without a cut (reach) */
    int seed = -1; float best = 1e30f;
    for (int g = 0; g < n; g++){ float d = hypotf(W.x[g] - START_X, W.y[g] - START_Y); if (d < best){ best = d; seed = g; } }
    for (int pass = 0; pass < 2; pass++){
        memset(deg, 0, sizeof(int) * ((size_t)n + 1));
        for (int s = 0; s < W.nseg; s++) if (pass == 0 || !W.scut[s]){ deg[W.sa[s] + 1]++; deg[W.sb[s] + 1]++; }
        for (int g = 0; g < n; g++) deg[g + 1] += deg[g];
        int *fill = malloc(sizeof(int) * (size_t)n);
        if (!fill){ fprintf(stderr, "[walls] f=%ld out of memory\n", f); break; }
        memcpy(fill, deg, sizeof(int) * (size_t)n);
        for (int s = 0; s < W.nseg; s++) if (pass == 0 || !W.scut[s]){ adj[fill[W.sa[s]]++] = W.sb[s]; adj[fill[W.sb[s]]++] = W.sa[s]; }
        free(fill);
        uint8_t mark = pass == 0 ? 1 : 2;
        int qh = 0, qt = 0, cnt = 0;
        if (seed >= 0 && (pass == 0 || seen[seed] == 1)){ seen[seed] = mark; queue[qt++] = seed; }
        while (qh < qt){
            int u = queue[qh++]; cnt++;
            for (int i = deg[u]; i < deg[u + 1]; i++){
                int v = adj[i];
                if (pass == 0 ? !seen[v] : seen[v] == 1){ seen[v] = mark; queue[qt++] = v; }
            }
        }
        if (pass == 0) W.npast = cnt; else W.nreach = cnt;
    }
    for (int g = 0; g < n; g++) W.reach[g] = seen[g] == 2 ? W_OPEN : seen[g] == 1 ? W_PAST : W_UNKNOWN;
    int joined = W.npast; W.npast = joined - W.nreach;
    free(deg); free(adj); free(queue); free(seen);
    /* the start's roads reach most of what joins them, or something is not as this expects: left alone */
    W.ok = seed >= 0 && best < 200.f && joined > n / 2 && W.nreach * 5 > joined;
    fprintf(stderr, "[walls] f=%ld roads: %d nodes, %d segments (%d joints), %d barrier lines on, %d segments cut; the start reaches %d "
            "nodes, %d past the career's walls, %d off its network%s\n", f, n, W.nseg, njoint, W.nbar, ncut, W.nreach, W.npast,
            n - joined, W.ok ? "" : ": not as expected, left as the game has it");
}

/* the network as the roads and the barriers are now (a new world, a new stage: built again) */
static void ensure(long f){
    if (!W.built || signature() != W.sig) build(f);
}

typedef struct {
    int state;            /* W_*: the place's */
    int seg; float d;     /* its nearest road segment, how far (metric) */
    int found; float qx, qy, qz, qd, hx, hy;   /* the nearest place on the career's roads WALL_GAP from a barrier, how far, the road's way */
} Where;

/* the nearest place on the career's roads to (x, y, z), at least WALL_GAP from a barrier line */
static void career_place(float x, float y, float z, Where *w){
    w->found = 0; w->qd = 1e30f;
    static const float TS[5] = { 0.f, .25f, .5f, .75f, 1.f };
    for (int s = 0; s < W.nseg; s++){
        int a = W.sa[s], b = W.sb[s];
        if (W.scut[s] || W.reach[a] != W_OPEN || W.reach[b] != W_OPEN) continue;
        float t0; float d = seg_dist(x, y, W.x[a], W.y[a], W.x[b], W.y[b], &t0);
        if (d >= w->qd) continue;                        /* (no point of it is nearer) */
        for (int k = -1; k < 5; k++){
            float t = k < 0 ? t0 : TS[k];
            float px = W.x[a] + t * (W.x[b] - W.x[a]), py = W.y[a] + t * (W.y[b] - W.y[a]), pz = W.z[a] + t * (W.z[b] - W.z[a]);
            float m = metric(hypotf(px - x, py - y), pz - z);
            if (m >= w->qd || bar_dist(px, py) < WALL_GAP) continue;
            float hx = W.x[b] - W.x[a], hy = W.y[b] - W.y[a], hl = hypotf(hx, hy);
            if (hl < 1e-3f) continue;
            w->found = 1; w->qd = m; w->qx = px; w->qy = py; w->qz = pz; w->hx = hx / hl; w->hy = hy / hl;
        }
    }
}

static void where(float x, float y, float z, Where *w){
    memset(w, 0, sizeof *w); w->seg = -1; w->d = 1e30f; w->state = W_UNKNOWN;
    for (int s = 0; s < W.nseg; s++){
        int a = W.sa[s], b = W.sb[s]; float t;
        float d2 = seg_dist(x, y, W.x[a], W.y[a], W.x[b], W.y[b], &t);
        if (d2 > w->d) continue;
        float m = metric(d2, W.z[a] + t * (W.z[b] - W.z[a]) - z);
        if (m < w->d){ w->d = m; w->seg = s; }
    }
    if (w->seg < 0) return;
    int s = w->seg, a = W.sa[s], b = W.sb[s];
    if (W.reach[a] == W_UNKNOWN && W.reach[b] == W_UNKNOWN) return;
    if (!W.scut[s] && W.reach[a] == W_OPEN && W.reach[b] == W_OPEN){ w->state = W_OPEN; return; }
    if (W.scut[s] && (W.reach[a] == W_OPEN || W.reach[b] == W_OPEN)){
        /* the segment a barrier line cuts: on its career end's side of that line, the place is on the career's roads */
        int e = W.reach[a] == W_OPEN ? a : b, i = crosses_any(x, y, W.x[e], W.y[e]);
        if (!i){ w->state = W_OPEN; return; }
    }
    /* past the walls, unless one of the career's roads is about as near, at its height, and on its side of them
     * (a road of the career's beside it, or over it with the car on that) */
    career_place(x, y, z, w);
    if (!w->found){ w->state = W_OPEN; return; }       /* (nowhere to go: left as it is) */
    w->state = w->qd > w->d + NEAR_MORE || fabsf(w->qz - z) > HEIGHT_SLACK + 2.f || crosses_any(x, y, w->qx, w->qy) ? W_PAST : W_OPEN;
}

static uint16_t heading_of(float hx, float hy){
    float a = atan2f(hy, hx);
    if (a < 0.f) a += 6.28318531f;
    return (uint16_t)(int)(a * (65536.f / 6.28318531f));
}

static const char *const WHY[] = { "", "free roam began there", "after a reset" };
enum { WHY_ENTRY = 1, WHY_RESET };

/* the car past the walls: to the nearest place on the career's roads (1: it goes) */
static int check(long f, uint32_t car, int why){
    ensure(f);
    if (!W.ok) return 0;
    float x = MRF32(car + CAR_PLACE), y = MRF32(car + CAR_PLACE + 4u), z = MRF32(car + CAR_PLACE + 8u);
    if (!isfinite(x + y + z) || fabsf(x) > PLACE_MAX || fabsf(y) > PLACE_MAX) return 0;   /* (car_rescue's) */
    Where w; where(x, y, z, &w);
    if (w.state != W_PAST) return 0;
    /* facing along the road, away from where the car was (the wall is that way) */
    float hx = w.hx, hy = w.hy;
    if ((w.qx - x) * hx + (w.qy - y) * hy < 0.f){ hx = -hx; hy = -hy; }
    uint16_t h = heading_of(hx, hy);
    MRF32(G_HOTPOS) = w.qx; MRF32(G_HOTPOS + 4u) = w.qy; MRF32(G_HOTPOS + 8u) = w.qz + 1.f;
    MR16(G_HOTPOS_ANGLE) = h; MR8(G_HOTPOS_VALID) = 1; MR8(G_HOTPOS_JUMP) = 1;
    roam_game_jumped();
    S.jumped_at = f;
    if (++S.jumps <= 20)
        fprintf(stderr, "[walls] f=%ld my car is past the career's walls (at %.1f %.1f %.1f, %s): back to %.1f %.1f %.1f, heading %04x, "
                "%.0f m away\n", f, (double)x, (double)y, (double)z, WHY[why], (double)w.qx, (double)w.qy, (double)w.qz, h, (double)w.qd);
    return 1;
}

/* travel.c (a trip to a friend in the city): whether `at` is on this career's side of its walls, and if so where its
 * car may go near `near`: the nearest place on its roads at least WALL_GAP from a barrier line (out, heading *h along
 * the road, toward (tx, ty)), 1; 0 when `at` is past its walls (a part of the city its career has not opened), or
 * the roads are not as expected (then nowhere is known to be open) */
int walls_open_near(const float at[3], const float near[3], float tx, float ty, float out[3], uint16_t *h){
    ensure(autotest_frame());
    if (!W.ok) return 0;
    Where w; where(at[0], at[1], at[2], &w);
    if (w.state == W_PAST) return 0;
    career_place(near[0], near[1], near[2], &w);
    if (!w.found) return 0;
    float hx = w.hx, hy = w.hy;
    if ((tx - w.qx) * hx + (ty - w.qy) * hy < 0.f){ hx = -hx; hy = -hy; }
    out[0] = w.qx; out[1] = w.qy; out[2] = w.qz;
    *h = heading_of(hx, hy);
    return 1;
}

/* the main-loop hook (roam.c), every frame */
void walls_frame(void){
    long f = autotest_frame();
    uint32_t car = 0;
    if (MR32(G_GAMEMODE) == 1u && MR32(G_RACETYPE) == 0u && MR32(G_FLOW) == 6u && MR32(G_RACEPARAMS) == 4000u) car = player_car();   /* the city (a race track's map, travel.c, has no career walls) */
    if (!car){ S.in = 0; return; }
    if (!S.in){
        memset(&S, 0, sizeof S); S.in = mprace_car_ok() ? 1 : 2; S.since = f; S.jumped_at = -100000; S.stamp = MR32(car + CAR_RESET_T);
    }
    if (S.in != 1) return;
    if (MR8(G_HOTPOS_JUMP)){ S.stamp = MR32(car + CAR_RESET_T); return; }   /* a jump on its way (ours, a rescue, a script) */
    float wdt = MRF32(G_WORLDDT);
    int running = isfinite(wdt) && wdt > 0.f, why = 0;
    if (!S.entry_done && running && f - S.since >= ENTRY_FRAMES){ S.entry_done = 1; why = WHY_ENTRY; }
    uint32_t st = MR32(car + CAR_RESET_T);
    if (st && st != S.stamp) why = WHY_RESET;
    S.stamp = st;
    if (why && f - S.jumped_at >= JUMP_PAUSE) check(f, car, why);
}

/* script command (tests): "walls" the car's place, "walls at X Y Z" a place's, "walls bars" the barrier lines on,
 * "walls sweep" places on and beside the roads, "walls fling VX VY VZ" the car's velocity (as a hit could leave it:
 * its rigid body's momentum) */
int walls_cmd(const char *args){
    long f = autotest_frame();
    char sub[32] = {0}; float a = 0, b = 0, c = 0;
    sscanf(args, "%31s %f %f %f", sub, &a, &b, &c);
    ensure(f);
    uint32_t car = player_car();
    if (!strcasecmp(sub, "bars")){
        for (int i = 0; i < W.nbar; i++)
            fprintf(stderr, "[walls] f=%ld barrier line %d: %.1f %.1f -> %.1f %.1f\n", f, i, (double)W.bar[i][0], (double)W.bar[i][1],
                    (double)W.bar[i][2], (double)W.bar[i][3]);
        return 1;
    }
    if (!strcasecmp(sub, "sweep") && W.ok){
        /* places on and beside the roads (every third node, on it and 6 m to each side): how many of the career's
         * roads' places would go, and how many past the walls would stay */
        static const float OFF[5][2] = { { 0, 0 }, { 6, 0 }, { -6, 0 }, { 0, 6 }, { 0, -6 } };
        int n[3][3] = { { 0 } }, said = 0;
        for (int g = 0; g < W.n; g += 3){
            if (W.reach[g] == W_UNKNOWN) continue;
            for (int k = 0; k < 5; k++){
                Where w; where(W.x[g] + OFF[k][0], W.y[g] + OFF[k][1], W.z[g] + 0.5f, &w);
                n[W.reach[g]][w.state]++;
                if (W.reach[g] == W_OPEN && w.state == W_PAST && said++ < 10)
                    fprintf(stderr, "[walls] f=%ld sweep: %.1f %.1f %.1f beside the career's roads would go (%.1f m from a barrier line)\n", f,
                            (double)(W.x[g] + OFF[k][0]), (double)(W.y[g] + OFF[k][1]), (double)W.z[g], (double)bar_dist(W.x[g], W.y[g]));
            }
        }
        fprintf(stderr, "[walls] f=%ld sweep: beside the career's roads %d stay, %d would go; beside the roads past its walls %d would go, "
                "%d stay\n", f, n[W_OPEN][W_OPEN] + n[W_OPEN][W_UNKNOWN], n[W_OPEN][W_PAST], n[W_PAST][W_PAST], n[W_PAST][W_OPEN] + n[W_PAST][W_UNKNOWN]);
        return 1;
    }
    if (!strcasecmp(sub, "fling")){
        uint32_t mb = car ? guest_rd32(car + CAR_RB) : 0, rb = guest_ok(mb, 0x6c) && MR32(mb + 0x5cu) ? guest_rd32(mb + 0x68u) : 0;
        if (!guest_ok(rb, 0x168)){ fprintf(stderr, "[walls] f=%ld fling: no rigid body\n", f); return 1; }
        float m = MRF32(rb + 0x164u), v[3] = { a, b, c };
        for (uint32_t i = 0; i < 3u; i++){ MRF32(rb + 0x70u + 4u * i) = v[i] * m; MRF32(rb + 0xa0u + 4u * i) = v[i]; MRF32(car + 0x70u + 4u * i) = v[i]; }
        fprintf(stderr, "[walls] f=%ld fling: velocity %.1f %.1f %.1f (mass %.3f)\n", f, (double)a, (double)b, (double)c, (double)m);
        return 1;
    }
    float x = a, y = b, z = c;
    if (strcasecmp(sub, "at")){
        if (!car){ fprintf(stderr, "[walls] f=%ld no car\n", f); return 1; }
        x = MRF32(car + CAR_PLACE); y = MRF32(car + CAR_PLACE + 4u); z = MRF32(car + CAR_PLACE + 8u);
        fprintf(stderr, "[walls] f=%ld my car: heading %04x, forward %.3f %.3f (heading of it %04x), %d jumps of ours\n", f,
                MR16(car + CAR_HEADING), (double)MRF32(car + CAR_FWD), (double)MRF32(car + CAR_FWD + 4u),
                heading_of(MRF32(car + CAR_FWD), MRF32(car + CAR_FWD + 4u)), S.jumps);
    }
    if (!W.ok){ fprintf(stderr, "[walls] f=%ld %.1f %.1f %.1f: no usable roads\n", f, (double)x, (double)y, (double)z); return 1; }
    Where w; where(x, y, z, &w);
    float rz = w.seg >= 0 ? W.z[W.sa[w.seg]] : 0.f;
    career_place(x, y, z, &w);
    fprintf(stderr, "[walls] f=%ld %.1f %.1f %.1f: %s (the nearest road %.1f m, its height %.1f%s); the nearest place on the career's "
            "roads %.1f %.1f %.1f, %.1f m\n", f, (double)x, (double)y, (double)z, STATE[w.state], (double)w.d, (double)rz,
            w.seg >= 0 && W.scut[w.seg] ? ", a barrier line across it" : "", (double)w.qx, (double)w.qy, (double)w.qz, (double)w.qd);
    return 1;
}
