/* tunegraph.c -- the ECU and turbo tuning graphs take a drag.
 *
 * The performance tuning screen UI_PerformanceTuning_Graph.fng (TuningGraphMenu,
 * vtable 0x79d374, message handler 0x5654d0) holds a graph at +0x50.  For ECU
 * and turbo it is the torque graph (vtable 0x79beb8): a column per 1000 rpm
 * (a ring at +0x20, the selected one at +0x28), each a slider (+8: value +0xc,
 * range +0x10 .. +0x14, a tenth of it a step) drawn as a bar with a dot on top
 * (the scale +0x18 .. +0x1c over +0x44 units up from +0x3c; left +0x38, width
 * +0x40, in 640x480 units around the centre).  Left and Right pick a column (method
 * 7, 0x522eb0), Up and Down move its dot a step (PAD_UP 0x72619778, PAD_DOWN
 * 0x911c0a4b: method 8, 0x522f10, then the screen's 0xe15f62f8).  The mouse
 * (message 0x9803f6e2, every frame: method 9, 0x522f90) picks the column
 * under the pointer and moves its dot with the wheel; a click does nothing.
 * A phone has neither wheel nor keys there, so a tap picked a column and
 * nothing moved its dot (a player's report from Android, 2026-10-24: "cannot
 * be regulated").  Nitrous's graph (vtable 0x79b7f4) has arrows to tap.
 *
 * Here: a press on the torque graph keeps the column it picked until the
 * button goes up, and every 8 units the pointer goes up or down from there
 * is an Up or a Down for the screen, as a key would be (a step moves the dot
 * some 7 units).  The press need not be on the dot, and the finger need not
 * hide it.  At the dot's end the pointer's place counts as the start again,
 * so turning back moves it after 8 units.  Original: the pointer only picks
 * the column. */
#include "cpu.h"
#include "guest.h"
#include <stdio.h>

void dispatch_patch(uint32_t a, void (*f)(CPU *));
void sub_005654d0(CPU *c);

#define FN_TUNE_MSG  0x005654d0u   /* thiscall (msg, obj, a3, a4), ret 0x10 */
#define VT_TORQUE    0x0079beb8u
#define MSG_MOUSE    0x9803f6e2u
#define MSG_UP       0x72619778u
#define MSG_DOWN     0x911c0a4bu
#define SC_GRAPH     0x50u
#define G_RING       0x20u         /* the columns: a ring, this its head */
#define G_SEL        0x28u
#define P_VALUE      0x0cu
#define P_X          0x38u
#define P_Y          0x3cu         /* the foot */
#define P_W          0x40u
#define P_H          0x44u
#define P_NUM        0x48u
#define MOUSE        0x008763d0u   /* +4 x, +8 y (640x480); +0x20 buttons swapped; +0x21 left down, +0x22 went down; right +3 */
#define STEP_UNITS   8

static uint32_t g_drag;            /* the screen whose graph the button holds, 0: none */
static int g_inner;                /* the game's own handling of the mouse runs */
static int g_ref, g_steps;         /* the pointer's y the next step counts from; steps so far */

static int pointer_y(void){ return (int32_t)MR32(MOUSE + 8u); }

/* the pointer is on one of the graph's columns, between its foot and its top */
static int on_graph(uint32_t g){
    float x = (float)(int32_t)MR32(MOUSE + 4u) - 320.f, y = (float)pointer_y() - 240.f;
    uint32_t head = g + G_RING;
    for (uint32_t p = MR32(head); p && p != head; p = MR32(p))
        if (x >= MRF32(p + P_X) && x <= MRF32(p + P_X) + MRF32(p + P_W) &&
            y <= MRF32(p + P_Y) && y >= MRF32(p + P_Y) - MRF32(p + P_H)) return 1;
    return 0;
}

/* a step for every 8 units the pointer has gone up or down, as Up and Down to the screen */
static void follow(CPU *c, uint32_t self, uint32_t p){
    int y = pointer_y();
    while (g_ref - y >= STEP_UNITS || y - g_ref >= STEP_UNITS){
        int up = g_ref > y;
        uint32_t a[4] = { up ? MSG_UP : MSG_DOWN, 0, 0, 0 };
        float v = MRF32(p + P_VALUE);
        gcall(c, FN_TUNE_MSG, self, 4, a);
        if (MRF32(p + P_VALUE) == v){ g_ref = y; break; }   /* at its end */
        g_ref += up ? -STEP_UNITS : STEP_UNITS;
        g_steps += up ? 1 : -1;
    }
}

static void tune_msg(CPU *c){
    uint32_t self = c->ecx, g = MR32(self + SC_GRAPH);
    if (g_inner || MR32(c->esp + 4u) != MSG_MOUSE || !g || MR32(g) != VT_TORQUE){ sub_005654d0(c); return; }
    uint32_t b = MOUSE + (MR8(MOUSE + 0x20u) ? 0x24u : 0x21u);   /* the main button, as 0x52d100 takes it */
    uint32_t p = MR32(g + G_SEL);
    if (g_drag && (g_drag != self || !MR8(b) || !p)){
        if (g_steps && p) fprintf(stderr, "[tune] column %u: %+d steps by a drag\n", MR32(p + P_NUM), g_steps);
        g_drag = 0;
    }
    if (g_drag) follow(c, self, p);
    else {
        uint32_t a[4] = { MR32(c->esp + 4u), MR32(c->esp + 8u), MR32(c->esp + 12u), MR32(c->esp + 16u) };
        g_inner = 1; gcall(c, FN_TUNE_MSG, self, 4, a); g_inner = 0;
        if (MR8(b + 1u) && MR32(g + G_SEL) && on_graph(g)){ g_drag = self; g_ref = pointer_y(); g_steps = 0; }
    }
    POP32(c->eip); c->esp += 16u;
}

void tunegraph_install(void){ dispatch_patch(FN_TUNE_MSG, tune_msg); }
