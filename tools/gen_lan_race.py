#!/usr/bin/env python3
"""gen_lan_race.py -- input scripts for LAN races between game instances
(tests/net/vlan_run.sh, tests/net/loop_run.sh).

  tools/gen_lan_race.py host|join [options] > script.txt

host: main menu -> LAN -> Host LAN Server (EA's server.dll) -> name -> LAN
      Main -> Select Car -> Lobby -> Create Game (mode, track, options) ->
      game room; waits for the joiners, clicks Start Race.
join: main menu -> LAN -> waits for the server list, joins the first server
      -> name -> LAN Main -> Select Car -> Lobby -> Join Game (the first game)
      -> game room -> Ready.
Both then wait for the race (GameFlow [0x8654a4] == 6, HUD_SingleRace.fng),
hand the car to the game's own AI driver (script "autopilot on") and dump
frames until the end of the race, then the post-race screens.

Options:
  --name NAME          LAN name (host: also the server name)
  --car N              Select Car: N taps RIGHT (negative: LEFT) from the
                       first car (default 0: the first car)
  --mode N             Create Game: N taps RIGHT on the mode carousel
                       (0 Circuit, 1 Sprint, 2 Drag, 3 Street X? -- read the
                       dump "mode"; host only)
  --track N            N taps RIGHT on the track list (host only)
  --opt KEYS           taps on the options screen, e.g. "DOWN,DOWN,RIGHT";
                       "dump:LABEL" dumps a frame in between (host only)
  --wait-room MS       host: real time in the game room before Start Race
                       (joiners must be Ready by then; default 70000)
  --join-wait MS       join: real time at the server list (default 40000)
  --lobby-wait MS      join: real time in the lobby before Join Game
                       (default 15000)
  --race-ms MS         time of periodic race dumps after the start (default
                       300000); then it waits for the results screen
                       (UI_PostRaceResults.fng) and dumps it
  --dump-every MS      race dump period (default 20000)
  --no-autopilot       keep the driver human (then nobody drives)
  --drag               drive with the synthetic pad (RT held, A = shift up
                       every 1.2 s): the AI driver does not drive drag cars
  --quit-mid MS        leave the race MS after the start (the process quits:
                       sockets closed, nothing more sent)
  --results-tmo N      frames to wait for the results screen (default 30000)
  --pause-at MS        open the in-race pause menu at MS (dumps), close it
  --refresh            join: press Refresh before Join Game.  The lobby lists
                       the games that existed at this player's own search, so
                       a host that creates its game after the joiner entered
                       the lobby (a slow or loaded machine) is only seen then
  --confirm-join       join: answer Yes to the game's "connection quality is
                       below average ... Are you sure?" question (slow links)
  --fps N              the instance's frame rate (default 60): every frame
                       count in the script is scaled by N/60
  --leave-mid MS       leave the race MS after the start through the pause
                       menu ("Quit to LAN Main Menu"), stay online 43 s, quit
  --after LINES        extra script lines (';'-separated) appended after the
                       race start, e.g. for robustness scenarios
  --race-end LINES     script lines (';'-separated) after the race dumps
  --filter N           join: set the lobby Filter's Game Mode first (N taps
                       RIGHT from Circuit: 1 Sprint, 2 Drag, ...); the lobby
                       lists only games of that mode
  --churn N            join: join the game room and leave it (ESCAPE) N
                       times before the final join (robustness)
  --lobby-churn N      join: leave the lobby for LAN Main and come back N
                       times first (robustness)
  --races N            N races in one session (default 1): after the
                       results everyone is back in the LAN Lobby; the host
                       creates a new game, taking what the mode and track
                       screens offer (circuit / the first track were used:
                       other modes UNVERIFIED) and the options as they are
                       (the screen keeps the previous laps, so --opt is not
                       repeated); the joiners press Refresh (the lobby does
                       not list new games by itself) and join again
"""
import argparse, sys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('role', choices=['host', 'join'])
    ap.add_argument('--name', default=None)
    ap.add_argument('--car', type=int, default=0)
    ap.add_argument('--mode', type=int, default=0)
    ap.add_argument('--track', type=int, default=0)
    ap.add_argument('--opt', default='')
    ap.add_argument('--wait-room', type=int, default=70000)
    ap.add_argument('--join-wait', type=int, default=40000)
    ap.add_argument('--lobby-wait', type=int, default=15000)
    ap.add_argument('--race-ms', type=int, default=300000)
    ap.add_argument('--dump-every', type=int, default=20000)
    ap.add_argument('--no-autopilot', action='store_true')
    ap.add_argument('--drag', action='store_true')
    ap.add_argument('--after', default='')
    ap.add_argument('--race-end', default='')
    ap.add_argument('--filter', type=int, default=0)
    ap.add_argument('--quit-mid', type=int, default=0)
    ap.add_argument('--results-tmo', type=int, default=30000)
    ap.add_argument('--pause-at', type=int, default=0)
    ap.add_argument('--leave-mid', type=int, default=0)
    ap.add_argument('--fps', type=int, default=60)
    ap.add_argument('--confirm-join', action='store_true')
    ap.add_argument('--churn', type=int, default=0)
    ap.add_argument('--lobby-churn', type=int, default=0)
    ap.add_argument('--races', type=int, default=1)
    ap.add_argument('--refresh', action='store_true')
    a = ap.parse_args()
    if a.races > 1 and (a.quit_mid or a.leave_mid or a.pause_at):
        ap.error('--races does not combine with --quit-mid, --leave-mid or --pause-at')
    host = a.role == 'host'
    name = a.name or ('HOSTMAC' if host else 'JOINER')
    L = []
    w = L.append
    w(f'# LAN race, {a.role} side, generated by tools/gen_lan_race.py {" ".join(sys.argv[1:])}')
    w('0     tapuntil RETURN MC_Background.fng 120 4000')
    w('+200  tap RETURN')
    w('+0    waitfng GarageMain.fng 3000')
    w('+300  tap RIGHT')
    w('+60   tap RIGHT')
    w('+60   tap RIGHT')                      # Quick Race -> Split Screen -> Online -> LAN
    w('+120  tap RETURN')
    if host:
        # the Host LAN Server button has no key; wait until "Looking for LAN
        # servers" has closed (8 s of real time, see lan_host.txt)
        w('+8000ms click 0.407 0.919')
    else:
        w(f'+{a.join_wait}ms dump list')
        w('+0    tap DOWN')
        w('+60   tap RETURN')
    w('+0    waitfng UI_PC_LAN.fng 900')
    w('+30   tap BACK')
    for _ in range(3): w('+10   tap BACK')
    w(f'+30   type {name}')
    w('+60   tap RETURN')
    w('+0    waitfng UI_OLMAIN.fng 1500')
    # Select Car: RIGHT, RETURN -> UI_OLCarSelect; pick; "race with this
    # vehicle?" defaults to No (LEFT = Yes); Transmission defaults to Auto
    w('+300  tap RIGHT')
    w('+60   tap RETURN')
    w('+0    waitfng UI_OLCarSelect.fng 900')
    w('+180  log car-select')
    k = 'RIGHT' if a.car >= 0 else 'LEFT'
    for _ in range(abs(a.car)): w(f'+40   tap {k}')
    w('+90   dump car')
    w('+0    tap RETURN')
    w('+120  tap LEFT')
    w('+30   tap RETURN')
    w('+120  dump transmission')
    w('+0    tap RETURN')
    w('+0    waitfng UI_OLMAIN.fng 900')
    w('+120  tap LEFT')
    w('+60   tap RETURN')
    # the lobby screen is covered by a dialog in the next frame (no waitfng)
    w('+6000ms log lobby')
    if host:
        w('+0    log create-game')
        w('+0    tap RETURN')
        w('+0    waitfng UI_QRModeSelect.fng 900')
        w('+200  log mode')
        for _ in range(a.mode): w('+40   tap RIGHT')
        w('+60   dump mode')
        w('+0    tap RETURN')
        w('+0    waitfng UI_QRTrackSelect.fng 900')
        w('+200  log track')
        for _ in range(a.track): w('+40   tap RIGHT')
        w('+60   dump track')
        w('+0    tap RETURN')
        w('+0    waitfng UI_QRModeOptions.fng 900')
        w('+200  dump options_0')
        for kk in [x for x in a.opt.split(',') if x]:
            w(f'+40   dump {kk[5:] or "opt"}' if kk.startswith('dump') else f'+40   tap {kk}')
        w('+60   dump options_1')
        w('+0    tap RETURN')
        w('+0    waitfng PC_OL_GameRoom.fng 900')
        w('+300  dump room_0')
        w(f'+{a.wait_room}ms dump room_1')
        w('+0    log start-race')
        w('+0    click 0.311 0.172')
        w('+3000ms dump start')
    else:
        w(f'+{a.lobby_wait}ms dump lobby')
        # the lobby lists only games of the Filter's Game Mode (Circuit by
        # default: CUSTFLAGS in the game search); Filter is the 4th button
        if a.filter:
            w('+0    log filter')
            for _ in range(3): w('+40   tap RIGHT')
            w('+60   tap RETURN')
            w('+300  log filter-screen')
            for _ in range(a.filter): w('+40   tap RIGHT')
            w('+60   dump filter')
            w('+0    tap RETURN')
            w('+3000ms dump lobby_filtered')
            w('+0    log join-game')
            w('+0    tap RIGHT')
            w('+60   tap RETURN')
            w('+0    waitfng PC_OL_GameRoom.fng 900')
            w('+300  dump room_0')
            a.churn = a.lobby_churn = 0
            joined = True
        else:
            joined = False
        # robustness: leave the lobby for LAN Main and come back, N times
        for k in range(a.lobby_churn):
            w(f'+0    log lobby-churn {k + 1}')
            w('+0    tap ESCAPE')
            w('+0    waitfng UI_OLMAIN.fng 900')
            w(f'+120  dump lanmain_{k + 1}')
            w('+0    tap RETURN')
            w('+6000ms dump lobby_{}'.format(k + 1))
        # robustness: join the game room and leave it again, N times
        for k in range(a.churn):
            w(f'+0    log room-churn {k + 1}')
            w('+0    tap RIGHT')
            w('+60   tap RETURN')
            w('+0    waitfng PC_OL_GameRoom.fng 900')
            w(f'+300  dump churn_room_{k + 1}')
            # "Are you sure you want to exit this game session?" OK / Cancel
            # (default Cancel)
            w('+0    tap ESCAPE')
            w(f'+180  dump churn_leave_{k + 1}')
            w('+0    tap LEFT')
            w('+40   tap RETURN')
            w(f'+3000ms dump churn_lobby_{k + 1}')
        if not joined:
            if a.refresh:
                # Refresh is the third button, Join Game the second
                w('+0    log refresh')
                w('+0    tap RIGHT')
                w('+40   tap RIGHT')
                w('+60   tap RETURN')
                w('+3000ms dump refreshed')
                w('+0    log join-game')
                w('+0    tap LEFT')
            else:
                w('+0    log join-game')
                w('+0    tap RIGHT')
            w('+60   tap RETURN')
            if a.confirm_join:
                # "Your connection quality to this game is below average ...
                # Are you sure you want to join this game?" (default No)
                w('+240  dump join_quality')
                w('+0    tap LEFT')
                w('+40   tap RETURN')
            w('+0    waitfng PC_OL_GameRoom.fng 900')
            w('+300  dump room_0')
        w('+0    log ready')
        w('+0    click 0.311 0.172')
        w('+3000ms dump room_ready')
    race_start(w, a)
    for line in [x.strip() for x in a.after.split(';') if x.strip()]: w(line)
    t = 0
    end = a.quit_mid or a.leave_mid or a.race_ms
    while t < end:
        step = min(a.dump_every, end - t)
        t += step
        w(f'+{step}ms dump race_{t // 1000}')
        if t // a.dump_every % 3 == 1: w('+0    peek 0x00865558 1')
        if a.pause_at and t == a.pause_at:
            # the in-race pause menu (exploration): ESCAPE, look, ESCAPE
            w('+0    tap ESCAPE')
            w('+120  dump pause_0')
            w('+0    tap DOWN')
            w('+60   dump pause_1')
            w('+0    tap DOWN')
            w('+60   dump pause_2')
            w('+0    tap ESCAPE')
            w('+120  dump pause_end')
    if a.leave_mid:
        # robustness: this player leaves the race through the in-race pause
        # menu (its only item: "Quit to LAN Main Menu"), then stays online
        w('+0    log leave-mid-race')
        w('+0    tap ESCAPE')
        w('+120  dump leave_menu')
        w('+0    tap RETURN')
        # "Are you sure you want to quit to the LAN main menu?" OK / Cancel
        # (default Cancel)
        w('+120  dump leave_confirm')
        w('+0    tap LEFT')
        w('+40   tap RETURN')
        w('+180  dump leave_1')
        w('+3000ms dump leave_2')
        w('+10000ms dump leave_3')
        w('+30000ms dump leave_4')
        w('+60   quit')
        print('\n'.join(scale_frames(L, a.fps / 60.0))); return
    if a.quit_mid:
        # robustness: this player leaves in the middle of the race (the
        # process ends: sockets closed, nothing sent any more)
        w('+0    log quit-mid-race')
        w('+0    quit')
        print('\n'.join(scale_frames(L, a.fps / 60.0))); return
    # the end of the race: the game's results screen on every instance
    w(f'+0    waitfng UI_PostRaceResults.fng {a.results_tmo}')
    w('+0    log results')
    w('+180  dump results')
    w('+3000ms dump results_2')
    for r in range(2, a.races + 1):
        race_again(w, a, host, r)
    for line in [x.strip() for x in a.race_end.split(';') if x.strip()]: w(line)
    w('+60   quit')
    print('\n'.join(scale_frames(L, a.fps / 60.0)))

def race_again(w, a, host, r):
    """race r of the session: Continue from the results to the LAN Lobby,
    the host creates the next game, the joiners Refresh and join it"""
    x = f'_r{r}'
    w('+300  tap RETURN')
    w('+3000ms log continue-done')
    w(f'+0    log === race {r} ===')
    w('+6000ms log lobby')
    if host:
        w('+0    log create-game')
        w('+0    tap RETURN')
        w('+0    waitfng UI_QRModeSelect.fng 900')
        w(f'+260  dump mode{x}')
        w('+0    tap RETURN')
        w('+0    waitfng UI_QRTrackSelect.fng 900')
        w(f'+260  dump track{x}')
        w('+0    tap RETURN')
        w('+0    waitfng UI_QRModeOptions.fng 900')
        w(f'+260  dump options{x}')
        w('+0    tap RETURN')
        w('+0    waitfng PC_OL_GameRoom.fng 900')
        w(f'+300  dump room_0{x}')
        w(f'+{a.wait_room}ms dump room_1{x}')
        w('+0    log start-race')
        w('+0    click 0.311 0.172')
        w(f'+3000ms dump start{x}')
    else:
        # the host needs time to create its game; the list is only filled by
        # this player's own search: Refresh (third button), then Join Game
        w(f'+{a.lobby_wait + 10000}ms dump lobby{x}')
        w('+0    log refresh')
        w('+0    tap RIGHT')
        w('+40   tap RIGHT')
        w('+60   tap RETURN')
        w(f'+3000ms dump refreshed{x}')
        w('+0    log join-game')
        w('+0    tap LEFT')
        w('+60   tap RETURN')
        if a.confirm_join:
            w(f'+240  dump join_quality{x}')
            w('+0    tap LEFT')
            w('+40   tap RETURN')
        w('+0    waitfng PC_OL_GameRoom.fng 900')
        w(f'+300  dump room_0{x}')
        w('+0    log ready')
        w('+0    click 0.311 0.172')
        w(f'+3000ms dump room_ready{x}')
    race_start(w, a)
    t = 0
    while t < a.race_ms:
        step = min(a.dump_every, a.race_ms - t)
        t += step
        w(f'+{step}ms dump race_{t // 1000}{x}')
    w(f'+0    waitfng UI_PostRaceResults.fng {a.results_tmo}')
    w(f'+0    log results{x}')
    w(f'+180  dump results{x}')
    w(f'+3000ms dump results_2{x}')

def race_start(w, a):
    """from the game room to the race: wait for GameFlow 6, then drive"""
    w('+0    waitmem 0x008654a4 6 20000')
    w('+0    log race-loaded')
    w('+0    peek 0x00865558 1')
    if a.drag:
        # drag: the AI driver does not take a drag car; the synthetic pad
        # drives it (RT throttle held, A = shift up every 1.2 s)
        # throttle held through the intro and the countdown (revving);
        # shifts from about the start on
        w('+0    trigger RT 1')
        w('+0    wzero')
        w('+9000ms log race-start')
        w('+0    peek 0x00865558 1')
        for k in range(16):
            w('+1200ms tappad A')
    elif not a.no_autopilot:
        # the local player becomes a human driver at the race start
        # (Player::SetState): the autopilot waits for that
        w('+0    autopilot on 6000')
        w('+0    wzero')
        w('+0    log race-start')
        w('+0    peek 0x00865558 1')

def scale_frames(lines, k):
    """frame counts were written for 60 fps: scale "+N" delays (not "+Nms")
    and the frame arguments of tapuntil / waitfng / waitmem / autopilot"""
    if k == 1.0: return lines
    out = []
    for ln in lines:
        if ln.startswith('#'): out.append(ln); continue
        f = ln.split()
        if f and f[0].startswith('+') and f[0][1:].isdigit():
            f[0] = '+' + str(int(round(int(f[0][1:]) * k)))
        cmd = f[1] if len(f) > 1 else ''
        idx = {'tapuntil': (4, 5), 'waitfng': (3,), 'waitmem': (4,), 'autopilot': (3,)}.get(cmd, ())
        for i in idx:
            if i < len(f) and f[i].isdigit(): f[i] = str(int(round(int(f[i]) * k)))
        out.append(f[0].ljust(6) + ' ' + ' '.join(f[1:]) if f else ln)
    return out

if __name__ == '__main__':
    main()
