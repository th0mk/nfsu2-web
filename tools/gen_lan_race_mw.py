#!/usr/bin/env python3
"""gen_lan_race_mw.py -- Most Wanted's input scripts for LAN races between game
instances (web/tools/lan-pair.mjs --game mw; Underground 2's are
tools/gen_lan_race.py's).

  tools/gen_lan_race_mw.py host|join [options] > script.txt

Every step waits for its screen (NFSU2_FNG_LOG's names, runtime/mw/fe_mw.c),
never for time alone, apart from the joiner's wait for the host's game.

Both: a first start: the alias (MC_Main.fng: Create a new alias? Yes, the two
autosave notes, the name, save it: Yes), the main menu's LAN Play (its 5th
item, 0x4B4E388E): UI_PC_LAN_ServerSelect.fng with "Looking for LAN servers"
(Dialog.fng) over it.
host: once the search is over, key 1 (the screen's Host message,
      PAD_BUTTON1 0xC519BFC0 at 0x5607C0: [[0x91CF90]+0x172] = 1) opens
      UI_PC_LAN.fng, the player's name; Enter starts EA's server.dll
      (0x741E20: LoadLibrary("server"), StartServer(name)) and joins it
      (UI_OLISPConnect.fng, then OL_MAIN.fng).
join: waits until the host's server is listed (its name in the list), Enter
      joins it (UI_PC_LAN.fng, the name, Enter: OL_MAIN.fng).
OL_MAIN.fng's first item, Quick Race (OL_Quickrace_Main.fng), its race kind
(--kind N: N taps DOWN), Enter searches (OL_Find_Session.fng).  The host
searches first: "No suitable matches" (OL_Dialog_Stacked_Buttons.fng) and
Accept creates the game (OL_GameRoom.fng); a joiner searches after
--join-wait ms and joins that game.
In the room nobody presses anything: with the room's minimum of players in it
the game starts the race by itself (about a minute after the second player
came in, in the runs of 2026-10-29; Enter there opens the room's car select,
which the start does not wait for).  --room LINES (';'-separated) are steps
in the room all the same.  Then both wait for the race (GameFlow [0x925E90]
== 6), hand the car to the game's AI driver (autopilot) and wait for the
results screen (--results SCREEN).

Options:
  --name NAME          the alias and LAN name (host: the server's name too)
  --car N              unused yet (the room's car is the alias's first)
  --kind N             the Quick Race kind: N taps DOWN (host and joiners)
  --join-wait MS       join: real time on the kinds before searching (default
                       20000: the host has created its game by then)
  --room-wait MS       time in the game room before the --room steps (default
                       20000)
  --room LINES         script lines in the game room (none by default)
  --race-ms MS         time of the race before the results wait (default
                       120000)
  --results SCREEN     the results screen (waitfng name, default
                       PostRace_Results.fng)
  --results-tmo N      frames to wait for it (default 30000)
  --races N            races in a row (only 1 yet)
  --after LINES        extra script lines (';'-separated) after the race start
"""
import argparse
import sys

FLOW = '0x00925e90'          # GameFlowManager's state: 3 front end, 6 racing (GAME 13.5)


def alias(w, name):
    """a first start: make the alias NAME and save it, then the main menu"""
    w('0     waitmem %s 3 3600                          # the front end' % FLOW)
    w('+0    tapuntil RETURN MC_Main.fng 90 3000')
    w('+0    waitfng MC_Main.fng:be97590f@b8a7c6cc 600          # Create a new alias?  (No)')
    w('+0    tapuntil LEFT MC_Main.fng:be97590f@b8a7c6cd 30 300  # Yes')
    w('+0    tap RETURN')
    w('+0    waitfng MC_Main.fng:b01be5dd 600                   # the autosave notes')
    w('+0    tap RETURN')
    w('+0    waitfng MC_Main.fng:b380362f 600')
    w('+0    tap RETURN')
    w('+0    waitfng MC_Main.fng:4f876b6f 600                   # Enter name: NAME')
    w('+0    loop 4')
    w('+0    nstap BACK')
    w('+20   endloop')
    w('+0    nstype %s' % name)
    w("+0    waitfng MC_Main.fng'%s 600" % name)
    w('+0    tap RETURN                                         # Accept')
    w("+0    waitfng MC_Main.fng'Do_you_want_to_save@b8a7c6cc 600")
    w("+0    tapuntil LEFT MC_Main.fng'Do_you_want_to_save@b8a7c6cd 30 300")
    w('+0    tap RETURN')
    w("+0    waitfng MC_Main.fng'Save_complete. 600")
    w('+0    tap RETURN')
    w('+0    waitfng MainMenu.fng@4b4e388a 900                  # the main menu, on Career')
    w('+0    log lan: alias %s' % name)


def lan_name(w, name):
    """UI_PC_LAN.fng: the field holds NAME; the player's name, Enter"""
    w('+0    waitfng UI_PC_LAN.fng 900')
    w('+30   loop 8')
    w('+0    nstap BACK')
    w('+10   endloop')
    w('+0    nstype %s' % name)
    w('+60   tap RETURN')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('role', choices=['host', 'join'])
    ap.add_argument('--name', default=None)
    ap.add_argument('--car', type=int, default=0)
    ap.add_argument('--kind', type=int, default=0)
    ap.add_argument('--join-wait', type=int, default=20000)
    ap.add_argument('--room-wait', type=int, default=20000)
    ap.add_argument('--room', default='')
    ap.add_argument('--race-ms', type=int, default=120000)
    ap.add_argument('--results', default='PostRace_Results.fng')
    ap.add_argument('--results-tmo', type=int, default=30000)
    ap.add_argument('--races', type=int, default=1)
    ap.add_argument('--after', default='')
    a = ap.parse_args()
    if a.races != 1:
        ap.error('--races: one race only yet')
    host = a.role == 'host'
    name = a.name or ('HOSTWEB' if host else 'JOINWEB')
    L = []
    w = L.append
    w('# Most Wanted LAN race, %s side, generated by tools/gen_lan_race_mw.py %s' % (a.role, ' '.join(sys.argv[1:])))
    alias(w, name)
    w('+0    tapuntil RIGHT MainMenu.fng@4b4e388e 40 600        # LAN Play')
    w('+0    tap RETURN')
    w("+0    waitfng Dialog.fng'Looking_for_LAN@b8a7c6cc 900    # the search for LAN servers")
    w('+0    waitfng UI_PC_LAN_ServerSelect.fng:6f827182 1800   # over: the list takes input')
    if host:
        w('+0    log lan: host')
        w('+0    tap 1                                              # Host (PAD_BUTTON1)')
    else:
        w("+0    waitfng UI_PC_LAN_ServerSelect.fng'HOSTWEB 7200    # the host's server is listed")
        w('+0    log lan: join')
        w('+30   tap RETURN')
    lan_name(w, name)
    w('+0    waitfng OL_MAIN.fng@4b4e388a 3600                  # connected: the LAN main menu, on Quick Race')
    w('+0    log lan: lobby')
    w('+30   tap RETURN')
    w('+0    waitfng OL_Quickrace_Main.fng@4b4e388a 900')
    for _ in range(a.kind):
        w('+30   tap DOWN')
    if not host:
        w('+%dms log lan: search' % a.join_wait)
    w('+30   tap RETURN                                         # search (OL_Find_Session.fng)')
    if host:
        w("+0    waitfng OL_Dialog_Stacked_Buttons.fng'No_suitable@b8a7c6cc 3600   # No suitable matches (Accept)")
        w('+30   tap RETURN                                         # Accept: create the game')
    w('+0    waitfng OL_GameRoom.fng 3600')
    w('+0    log lan: room')
    w('+%dms log lan: start' % a.room_wait)
    for ln in [s.strip() for s in a.room.split(';') if s.strip()]:
        w(ln)
    w('+0    waitmem %s 6 36000                         # racing' % FLOW)
    w('+0    log lan: racing')
    w('+0    autopilot on')
    w('+0    log race-start')
    for ln in [s.strip() for s in a.after.split(';') if s.strip()]:
        w(ln)
    w('+%dms log lan: race time' % a.race_ms)
    w('+0    waitfng %s %d' % (a.results, a.results_tmo))
    w('+0    log lan: results')
    w('+300  close')
    sys.stdout.write('\n'.join(L) + '\n')


if __name__ == '__main__':
    main()
