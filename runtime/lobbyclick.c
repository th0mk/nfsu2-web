/* lobbyclick.c -- a click on a dialog over the Game Session Lobby is not also
 * a click on the game list under it.
 *
 * The lobby (UI_OLLobbyRoom, package PC_OL_Lobby.fng: Online and LAN; its
 * MessageHandler 0x4fcda0, called only from its vtable 0x795fd0) takes the
 * mouse on message 0x9803f6e2: 0x4faa60 selects the row under the pointer,
 * sorts on a column header or presses a button where the left button went
 * down this frame (0x52d100: the mouse 0x8763d0, updated by 0x5bf860: +0x20
 * buttons swapped, +0x21/+0x22/+0x23 left down / went down / went up, right
 * +0x24.., middle +0x27..).  Its Join (0x49ea80) asks first, online:
 * "Entering a game room may cause you to lose any unread messages ...", whose
 * OK (0x59337816) runs 0x4f1620, which joins the game of the row selected at
 * that moment (0x51f530: by its name).  The dialog takes the press on its OK
 * and closes in that frame, and the lobby got the same press in the same
 * frame: the row under the OK became the selection, and the join went to
 * that row's game and asked for its password if it had one (a player's
 * report, 2026-10-23: "It connects to a different lobby than the one
 * selected. It asks for a password on lobbies that don't have a password").
 * With 11 games or more listed a game's row lies under the OK; the rows under
 * the dialog are dimmed, so the new selection hardly shows.
 * The lobby already keeps the mouse off its list while its other questions
 * are up (+0x1cd: the connection quality question and the password screen,
 * set by 0x4f1620, cleared by their answers 0xbd6234af, 0x8905384,
 * 0xf7f19667); this question never set it.
 *
 * Here: once a dialog is up ([0x838974], seen on the lobby's messages of
 * every frame) the lobby's 0x9803f6e2 runs with +0x1cd set, until no dialog
 * is up and no mouse button is down.  Original: the click on OK also
 * selects the game under it. */
#include "cpu.h"
#include "game.h"

#define FN_LOBBY_MSG  0x004fcda0u   /* thiscall (msg, obj, a3, a4), ret 0x10 */
#define MSG_MOUSE     0x9803f6e2u   /* the lobby's mouse: 0x4faa60, unless +0x1cd */
#define LB_ASKING     0x1cdu        /* byte: one of the lobby's questions is up */
#define G_MOUSE_DOWN  0x008763f1u   /* left button down; right +3, middle +6 */

void sub_004fcda0(CPU *c);
void dispatch_patch(uint32_t a, void (*f)(CPU *));

static int g_dialog;   /* a dialog was up: the lobby's mouse waits for the buttons to go up */

static void lobby_msg(CPU *c){
    uint32_t self = c->ecx;
    if (MR32(G_DLG)) g_dialog = 1;
    else if (!MR8(G_MOUSE_DOWN) && !MR8(G_MOUSE_DOWN + 3u) && !MR8(G_MOUSE_DOWN + 6u)) g_dialog = 0;
    if (MR32(c->esp + 4u) != MSG_MOUSE || !g_dialog || MR8(self + LB_ASKING)){ sub_004fcda0(c); return; }
    MR8(self + LB_ASKING) = 1;
    sub_004fcda0(c);
    MR8(self + LB_ASKING) = 0;
}

void lobbyclick_install(void){ dispatch_patch(FN_LOBBY_MSG, lobby_msg); }
