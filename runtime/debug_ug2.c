/* UG2's debugging switches and test traces, moved out of the runtime's core
 * files (com.c, autotest.c): environment switches from the bring-up that read
 * and poke SPEED2.EXE's own globals, and the test runs' car trace and
 * front-end dump.  Core reaches them through runtime/game.h, game_ug2.c's
 * table: on_present (com.c's Present), on_sound_unlock (com.c's DirectSound
 * Unlock) and on_test_frame (autotest.c's frame). */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* resolve an environment variable once (com.c's ENVC) */
#define ENVC(nm_) ({ static const char *_ev_; static int _ei_; \
                     if (!_ei_){ _ei_ = 1; _ev_ = getenv(nm_); } _ev_; })

uint32_t com_present_no(void);
long autotest_frame(void);
const char *autotest_top_fng(void);

/* ---- every Present (on_present, com.c) ------------------------------------ */
void ug2_debug_present(CPU *c){
    uint32_t present_no = com_present_no();
    /* FETICK: the front-end tick [0x8651a8] should advance at 60 Hz, but
     * the game's frame-delta measurement always yields 1/1800 s, so the
     * tick never advances -> animations and timed screen transitions freeze.
     * Our virtual clock advances exactly 1/60 s per frame, so +1 tick
     * per frame gives the right rate. */
    if (ENVC("FETICK") && mem_is_committed(0x008651a8u))
        MR32(0x008651a8u) = MR32(0x008651a8u) + 1u;
    /* FIXFRAMETIME: the game's TARGET FRAME TIME [0x865558] is never
     * set (its setter sub_00576cb0/cd0 is never called) -> the frame
     * delta dt stays at 1/1800, the front-end tick [0x8651a8] never
     * advances, animations and timed screen transitions freeze.
     * The game's own default [0x7fb710] = 1/120. */
    { const char *ff = ENVC("FIXFRAMETIME");
      static int done_ft = 0;
      if (ff && *ff && ff[0] != '0' && !done_ft && present_no >= 2u){
          done_ft = 1;
          float v = (float)atof(ff);
          if (!(v > 0.f)) v = mem_is_committed(0x007fb710u)
                              ? MRF32(0x007fb710u) : (1.f/120.f);
          if (mem_is_committed(0x00865558u)){
              MRF32(0x00865558u) = v;
              fprintf(stderr,"  [frame-time] [0x865558] set to %g\n",
                      (double)v);
          }
      } }
    /* DATADIFF: which global variables change between two
     * frames?  A frozen counter/clock shows up directly. */
    { const char *dd2 = ENVC("DATADIFF");
      static uint8_t *snap = 0; static int taken = 0;
      const uint32_t DLO = 0x007E0000u, DHI = 0x008A0000u;
      if (dd2 && *dd2){
          uint32_t at = (uint32_t)atoi(dd2);
          if (!taken && present_no == at){
              snap = malloc(DHI - DLO);
              if (snap){ memcpy(snap, g_mem + DLO, DHI - DLO); taken = 1;
                         fprintf(stderr,"  [diff] frame %u: snapshot taken\n", at); }
          } else if (taken == 1 && present_no == at + 600u){
              taken = 2; int n = 0;
              fprintf(stderr,"  [diff] changed between frames %u and %u:\n", at, at+600u);
              for (uint32_t o = 0; o + 4 <= DHI - DLO; o += 4){
                  uint32_t oldv, newv;
                  memcpy(&oldv, snap + o, 4);
                  memcpy(&newv, g_mem + DLO + o, 4);
                  if (oldv == newv) continue;
                  if (++n > 40) break;
                  float fo, fn2; memcpy(&fo,&oldv,4); memcpy(&fn2,&newv,4);
                  fprintf(stderr,"     0x%08x: %10u -> %-10u  (%g -> %g)\n",
                          DLO + o, oldv, newv, (double)fo, (double)fn2);
              }
              fprintf(stderr,"  [diff] changed dwords in total (first 40 shown)\n");
          }
      } }
    /* FORCESWITCH: we were stuck at the splash.  Trigger the
     * transition by hand and measure whether the next screen
     * (profile / main menu) loads.  DIAGNOSTIC only. */
    { const char *fs2 = ENVC("FORCESWITCH");
      static int fired = 0;
      extern uint32_t g_fe_mgr;
      if (fs2 && *fs2 && !fired && present_no >= 2500u && g_fe_mgr){
          fired = 1;
          uint32_t from = 0x0078ea60u;              /* UG_LS_Splash.fng */
          uint32_t to   = (atoi(fs2) == 2) ? 0x0078f530u   /* UI_Main.fng */
                                           : 0x0078ea4cu;  /* MC_Background.fng */
          fprintf(stderr,"  [force] SwitchFEngPackages(manager=%08x, \"%s\" -> \"%s\")\n",
                  g_fe_mgr, (const char*)(g_mem+from), (const char*)(g_mem+to));
          { CPU save = *c;
            if (atoi(fs2) == 3){
                /* sub_004c4f50(current_package_name, 0): advances the
                 * boot flow; finds the target in its own list
                 * and switches BY LOADING it. */
                PUSH32(0u); PUSH32(from);
                PUSH32(0xDEADBEEFu);
                dispatch_indirect(c, 0x004c4f50u);
            } else {
                PUSH32(to); PUSH32(from);
                PUSH32(0xDEADBEEFu);
                c->ecx = g_fe_mgr;
                dispatch_indirect(c, 0x0051cba0u);
            }
            *c = save; }
          fprintf(stderr,"  [force] call returned\n");
      } }
    /* BOOTFLOW: walk the preload flow list (0x836488).
     * Shows directly what the splash screen is waiting for. */
    if (ENVC("BOOTFLOW") && (present_no % 900u) == 5u){
        uint32_t head = mem_is_committed(0x00836488u) ? MR32(0x00836488u) : 0;
        { /* Front-end clock chain:
           *   [0x865194] = frame delta (dt, seconds)
           *   [0x865140] = accumulator remainder
           *   [0x8651a8] = front-end tick (dt rounded to 60 Hz)
           * Animations and timed screen transitions hang off this tick. */
          fprintf(stderr,"  [fe-clock] frame %u: dt(865194)=%g  left(865140)=%g"
                  "  TICK(8651a8)=%d  frame(865148)=%d  target(865558)=%g\n", present_no,
                  mem_is_committed(0x00865194u)?(double)MRF32(0x00865194u):-1.0,
                  mem_is_committed(0x00865140u)?(double)MRF32(0x00865140u):-1.0,
                  mem_is_committed(0x008651a8u)?(int)MR32(0x008651a8u):-1,
                  mem_is_committed(0x00865148u)?(int)MR32(0x00865148u):-1,
                  mem_is_committed(0x00865558u)?(double)MRF32(0x00865558u):-1.0);
          { uint64_t host_us(void);
            uint64_t t1 = host_us(); volatile int sp=0;
            for (int q=0;q<200000;q++) sp+=q;
            uint64_t t2 = host_us();
            { /* __ftol (sub_0075bed4) accuracy: call it with
               * ST(0)=value and compare the result.  The only suspect
               * primitive in the frame-delta chain. */
              static int ft_done = 0;
              if (!ft_done){ ft_done = 1;
                const double tv[5] = { 90.5, 15.5, 1.5, 0.5, 30000.5 };
                for (int q = 0; q < 5; q++){
                    CPU sv = *c;
                    c->top = (c->top - 1) & 7; c->st[c->top] = tv[q];
                    PUSH32(0xDEADBEEFu);
                    dispatch_indirect(c, 0x0075bed4u);
                    fprintf(stderr,"           __ftol(%g) = %d %s\n",
                            tv[q], (int)c->eax,
                            ((int)c->eax == (int)tv[q]) ? "" : "  <-- WRONG");
                    *c = sv;
                } } }
            fprintf(stderr,"           STEP esi(86513c)=%d  left after int2(865144)=%g\n",
                    mem_is_committed(0x0086513cu)?(int)MR32(0x0086513cu):-1,
                    mem_is_committed(0x00865144u)?(double)MRF32(0x00865144u):-1.0);
            { /* sub_0057ead0 writes the measured frame time every
               * frame to 0x8647a8 + (frame%100)*16 + 4. */
              uint32_t fr = mem_is_committed(0x00865148u)?MR32(0x00865148u):0;
              fprintf(stderr,"           MEASURED frame time (last 4 frames):");
              for (int q=0;q<4;q++){
                  uint32_t idx = (fr - (uint32_t)q) % 100u;
                  uint32_t a4 = 0x008647a8u + idx*16u + 4u;
                  fprintf(stderr,"  %g", mem_is_committed(a4)?(double)MRF32(a4):-1.0);
              }
              fprintf(stderr,"\n"); }
            fprintf(stderr,"           ms/tick(828028)=%g  shift(8284e4)=%u\n",
                    mem_is_committed(0x00828028u)?(double)MRF32(0x00828028u):-1.0,
                    mem_is_committed(0x008284e4u)?MR32(0x008284e4u):0);
            fprintf(stderr,"           QPC divisor(8284e4)=%u  scale(78435c)=%g"
                    "  host_us within the frame: %llu -> %llu (delta %llu us)\n",
                    mem_is_committed(0x008284e4u)?MR32(0x008284e4u):0,
                    mem_is_committed(0x0078435cu)?(double)MRF32(0x0078435cu):0.0,
                    (unsigned long long)t1,(unsigned long long)t2,
                    (unsigned long long)(t2-t1)); }
          fprintf(stderr,"           input(7fb714)=%g  mode(7fb700)=%u  left2(865144)=%g\n",
                  mem_is_committed(0x007fb714u)?(double)MRF32(0x007fb714u):-1.0,
                  mem_is_committed(0x007fb700u)?MR32(0x007fb700u):0,
                  mem_is_committed(0x00865144u)?(double)MRF32(0x00865144u):-1.0); }
        { /* Game clock: sub_0057a1a0 => [0x865134] * constant.
           * If this counter does not advance, front-end animations freeze. */
          fprintf(stderr,"  [gameclock] tick(865134)=%d  (865138)=%08x  sel(7fb700)=%u"
                  "  scale=%g/%g  fe_t(864720)=%g\n",
                  mem_is_committed(0x00865134u)?(int)MR32(0x00865134u):-1,
                  mem_is_committed(0x00865138u)?MR32(0x00865138u):0,
                  mem_is_committed(0x007fb700u)?MR32(0x007fb700u):0,
                  mem_is_committed(0x007851c0u)?(double)MRF32(0x007851c0u):0.0,
                  mem_is_committed(0x007875bcu)?(double)MRF32(0x007875bcu):0.0,
                  mem_is_committed(0x00864720u)?(double)MRF32(0x00864720u):0.0); }
        { uint64_t host_us(void); uint32_t host_ms(void);
          fprintf(stderr,"  [clock] frame %u: host_us=%llu  host_ms=%u\n",
                  present_no, (unsigned long long)host_us(), host_ms()); }
        fprintf(stderr,"  [preload] frame %u  list=%08x  flag(83648c)=%u  (836498)=%08x\n",
                present_no, head,
                mem_is_committed(0x0083648cu) ? MR8(0x0083648cu) : 999u,
                mem_is_committed(0x00836498u) ? MR32(0x00836498u) : 0);
        if (head && mem_is_committed(head+0x40u)){
            for (uint32_t q = 0; q < 0x40u; q += 4u){
                uint32_t V = MR32(head+q);
                const char *t = 0;
                if (V > 0x10000u && mem_is_committed(V+40u)){
                    const char *z = (const char*)(g_mem+V);
                    int ok=1, L=0;
                    while (L<38 && z[L]){ unsigned char ch=(unsigned char)z[L];
                        if (ch<0x20u||ch>0x7Eu){ ok=0; break; } L++; }
                    if (ok && L>=3 && z[L]==0) t = z;
                }
                fprintf(stderr,"      +%02x = %08x%s%s\n", q, V,
                        t ? "  -> " : "", t ? t : "");
            }
        }
    }
    /* FLOWLOG: read the game's own flow tracker.
     * 0x86549c and 0x864f88 hold the NAME of the step running now. */
    if (ENVC("FLOWLOG") && (present_no % 500u) == 1u){
        static char last[160] = {0};
        char cur[160]; cur[0] = 0;
        uint32_t gl[2] = { 0x0086549cu, 0x00864f88u };
        for (int q = 0; q < 2; q++){
            const char *t = "(?)";
            if (mem_is_committed(gl[q]+3)){
                uint32_t P = MR32(gl[q]);
                if (P > 0x10000u && mem_is_committed(P+2)) t = (const char*)(g_mem+P);
            }
            strncat(cur, t, 70);
            strncat(cur, " | ", 4);
        }
        if (strcmp(cur, last)){
            snprintf(last, sizeof last, "%s", cur);
            fprintf(stderr,"  [flow] frame %u: %s  flag(865498)=%u (8654a4)=%u\n",
                    present_no, cur,
                    mem_is_committed(0x00865498u)?MR8(0x00865498u):999u,
                    mem_is_committed(0x008654a4u)?MR32(0x008654a4u):999u);
        }
    }
    /* The game's own slot pools: an exhausted one prints its name in
     * red on screen.  List 0x828538 (tail) -> +4 (previous) ... 0x828534. */
    if (ENVC("POOLS") && (present_no % 150) == 20){
        fprintf(stderr,"  --- slot pools (list head 0x828530=%u) ---\n",
                mem_is_committed(0x828530u) ? MR32(0x828530u) : 0xFFFFFFFFu);
        /* pools known directly: EventHandler (0x883e38), Event (0x883e34?) */
        { uint32_t known[] = {0x883e38u, 0x883e34u};
          for (int q=0;q<2;q++){
            if (!mem_is_committed(known[q]+4)) continue;
            uint32_t po = MR32(known[q]);
            if (!po || !mem_is_committed(po+0x2f)) continue;
            uint32_t np2 = MR32(po+0x0c);
            char nm3[48]; nm3[0]=0;
            if (np2 && mem_is_committed(np2))
                for (int k2=0;k2<40;k2++){ uint8_t ch=MR8(np2+k2);
                    if (ch<32||ch>126){ nm3[k2]=0; break; } nm3[k2]=(char)ch; nm3[k2+1]=0; }
            fprintf(stderr,"    [0x%08x] %-24s capacity=%u free=%u element=%u"
                    "  (+08=%u +10=%u +18=%u +1c=%u)\n",
                    known[q], nm3, MR32(po+0x24), MR32(po+0x2c), MR32(po+0x28),
                    MR32(po+8), MR32(po+0x10), MR32(po+0x18), MR32(po+0x1c));
          } }
        uint32_t n2 = mem_is_committed(0x828538u) ? MR32(0x828538u) : 0;
        int guard2 = 0;
        while (n2 && n2 != 0x828534u && guard2++ < 400){
            if (!mem_is_committed(n2+0x2f)) break;
            uint32_t nameptr = MR32(n2+0x0c);
            uint32_t cap = MR32(n2+0x24), esz = MR32(n2+0x28), fr = MR32(n2+0x2c);
            char nm2[48]; nm2[0]=0;
            if (nameptr && mem_is_committed(nameptr))
                for (int q=0;q<40;q++){ uint8_t ch=MR8(nameptr+q);
                    if (ch<32||ch>126){ nm2[q]=0; break; } nm2[q]=(char)ch; nm2[q+1]=0; }
            if (cap && cap < 100000u)
                fprintf(stderr,"    %-28s capacity=%-6u free=%-6u element=%u%s\n",
                        nm2, cap, fr, esz, (fr==0)?"   <<< EXHAUSTED":"");
            n2 = MR32(n2+4);
        }
    }
}

/* ---- every DirectSound buffer Unlock (on_sound_unlock, com.c) ------------- */
void ug2_debug_sound_unlock(void){
    /* VOICELOG=1: the game's OWN sound channel table.
     * sub_00725378 walks the table with address [0x8b466c], stride
     * 0x90, count [0x8b460c] (int16); it tests byte +0x26 as "in use".
     * The only way to measure directly whether SFX events open a
     * channel. */
    if (ENVC("VOICELOG")){
        static unsigned nu=0; static int peak=0;
        if ((nu++ % 120u) == 0u){
            uint32_t tb = mem_is_committed(0x8b466cu) ? MR32(0x8b466cu) : 0u;
            int cnt = mem_is_committed(0x8b460cu) ? (int)(int16_t)MR16(0x8b460cu) : 0;
            int act = 0;
            if (tb && cnt > 0 && cnt < 256)
                for (int q=0;q<cnt;q++){
                    uint32_t e = tb + (uint32_t)q*0x90u;
                    if (mem_is_committed(e+0x26u) && MR8(e+0x26u)) act++;
                }
            if (act > peak) peak = act;
            /* Change ACROSS the table: even if my reading (+0x26) is
             * wrong, this shows whether any field is being written. */
            unsigned nz = 0, ent_nz = 0;
            if (tb && cnt > 0 && cnt < 256)
                for (int q=0;q<cnt;q++){
                    uint32_t e = tb + (uint32_t)q*0x90u; unsigned any=0;
                    if (!mem_is_committed(e+0x8fu)) continue;
                    for (unsigned k=0;k<0x90u;k++) if (MR8(e+k)){ nz++; any=1; }
                    if (any) ent_nz++;
                }
            /* Volume globals: sub_0072d3ec scales the sound as
             * ((uint16)[0x8b44c6] << 8) * param. */
            fprintf(stderr,"  [level] [0x8b44c6]=%u  [0x8b44c4]=%u  [0x8b44c8]=%u"
                    "  [0x8b44ca]=%u  [0x8b44cc]=%u\n",
                    mem_is_committed(0x8b44c6u)?MR16(0x8b44c6u):0xFFFFu,
                    mem_is_committed(0x8b44c4u)?MR16(0x8b44c4u):0xFFFFu,
                    mem_is_committed(0x8b44c8u)?MR16(0x8b44c8u):0xFFFFu,
                    mem_is_committed(0x8b44cau)?MR16(0x8b44cau):0xFFFFu,
                    mem_is_committed(0x8b44ccu)?MR16(0x8b44ccu):0xFFFFu);
            fprintf(stderr,"  [channel] table=0x%08x count=%d active=%d (peak=%d)"
                    "  non-zero entries=%u bytes=%u\n",
                    tb, cnt, act, peak, ent_nz, nz);
        }
    }
}

/* ---- test runs (on_test_frame, autotest.c) -------------------------------- */
/* NFSU2_CAR_TRACE=<file>: every car of the race World per frame.
 * One CSV row per Present while racing (GameFlow 6): wall-clock time
 * (CLOCK_REALTIME, shared by every instance on this Mac), frame, whether
 * a race HUD (HUD_*.fng) is the top package (not the results), the frame cap
 * [0x865558], WorldTimer/RealTimer (1/4000 s), then per car of
 * the World's list ([0x890080]: +0x10 count, +0x1C car pointers) whether it
 * is the local player's (player 1's vehicle), position +0x60 and speed
 * +0x430.  tools/lan_trace.py compares how each instance shows the others'
 * cars with where their owners had them at the same instant (LAN tests). */
static FILE *g_ctrace;
static int g_fng_dump;                   /* NFSU2_FNG_LOG=2: the front end's objects at frames 400 and 1500 */
uint32_t autopilot_vehicle(void);        /* career.c: player 1's vehicle, 0 = none */
static void car_trace(long frame){
    static uint32_t last_w = 0;
    if (!mem_is_committed(0x8654a7u) || MR32(0x8654a4u) != 6u) return;
    uint32_t w = MR32(0x00890080u);
    if (!w || !mem_is_committed(w + 0x3f)) return;
    uint32_t n = MR32(w + 0x10); if (n > 8) n = 8;
    uint32_t me = autopilot_vehicle();
    if (w != last_w){
        last_w = w;
        fprintf(stderr, "[cartrace] f=%ld world 0x%08x cars %u, local vehicle 0x%08x:", frame, w, n, me);
        for (uint32_t i = 0; i < n; i++) fprintf(stderr, " 0x%08x", MR32(w + 0x1c + 4 * i));
        fputc('\n', stderr);
    }
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    const char *top = autotest_top_fng();
    fprintf(g_ctrace, "%lld.%06ld,%ld,%d,%.7f,%u,%u,%u", (long long)ts.tv_sec, (long)(ts.tv_nsec / 1000), frame,
            top && !strncasecmp(top, "HUD_", 4), (double)MRF32(0x00865558u), MR32(0x0086518cu),
            MR32(0x008651acu), n);
    for (uint32_t i = 0; i < n; i++){
        uint32_t car = MR32(w + 0x1c + 4 * i);
        if (car && mem_is_committed(car + 0x433))
            fprintf(g_ctrace, ",%d,%.3f,%.3f,%.3f,%.3f", car == me, (double)MRF32(car + 0x60), (double)MRF32(car + 0x64),
                    (double)MRF32(car + 0x68), (double)MRF32(car + 0x430));
        else fprintf(g_ctrace, ",,,,,");
    }
    fputc('\n', g_ctrace);
}
/* the FE manager [0x8384c4], its package manager (+8) and the top package's
 * FNGInfo (+0xE4), each word shown with the string it points at */
static void fng_dump(long frame){
    uint32_t fe = mem_is_committed(0x8384c4u) ? MR32(0x8384c4u) : 0;
    uint32_t mg = (fe && mem_is_committed(fe + 11)) ? MR32(fe + 8) : 0;
    uint32_t pk = (mg && mem_is_committed(mg + 0xe7)) ? MR32(mg + 0xe4) : 0;
    fprintf(stderr, "[fngdbg] f=%ld fe=%08x mgr=%08x info=%08x\n", frame, fe, mg, pk);
    for (int o = 0; pk && o < 0x100; o += 4){
        uint32_t v = MR32(pk + (uint32_t)o); char buf[40] = {0};
        if (v && mem_is_committed(v) && mem_is_committed(v + 39)){
            const char *q = (const char *)(g_mem + v); int n = 0;
            while (n < 39 && q[n] >= 0x20 && q[n] < 0x7f){ buf[n] = q[n]; n++; }
            if (n < 3 || q[n]) buf[0] = 0; }
        fprintf(stderr, "   +%02x %08x %s\n", o, v, buf);
    }
    for (int o = 0; fe && o < 0x40; o += 4) fprintf(stderr, "   fe+%02x %08x\n", o, MR32(fe + (uint32_t)o));
}
/* the trace file and the dump's switch are read at the first frame */
static void test_init(void){
    const char *ct = getenv("NFSU2_CAR_TRACE");
    if (ct && *ct && (g_ctrace = fopen(ct, "w"))){
        setvbuf(g_ctrace, 0, _IOLBF, 0);
        fprintf(g_ctrace, "t,frame,hud,cap,worldtimer,realtimer,n");
        for (int i = 0; i < 8; i++) fprintf(g_ctrace, ",c%dlocal,c%dx,c%dy,c%dz,c%dv", i, i, i, i, i);
        fputc('\n', g_ctrace);
    }
    const char *fl = getenv("NFSU2_FNG_LOG");
    g_fng_dump = fl && atoi(fl) == 2;
}
void ug2_debug_frame(void){
    static int inited;
    if (!inited){ inited = 1; test_init(); }
    long frame = autotest_frame();
    if (g_ctrace) car_trace(frame);
    if (g_fng_dump && (frame == 400 || frame == 1500)) fng_dump(frame);
}
