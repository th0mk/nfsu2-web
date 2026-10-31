#!/bin/bash
# Suite check for "status_race" (tests/mw/scripts/status_race.txt, TRACKER M6-15): the module under test is named
# (L-020), and runtime/mw/status_mw.c's [drive] lines (one at each change of the page's state) come in this order:
# the countdown (1 | 8, the HUD up before GO), driving (1), the pause menu (0, "pause" 1), driving again, the winner's
# screen (0), and nothing drives from the finish on; the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -qE '^\[autotest\] f=[0-9]+ .* TIMEOUT' "$LOG" && fail "a wait of the script ran out: $(grep -m1 -E '^\[autotest\] f=[0-9]+ .* TIMEOUT' "$LOG")"
miss=$(inorder 'hud=HUD_SingleRace.fng input=- race=1 go=0 done=0 pursuit=0 -> 9 pause=0' \
  'hud=HUD_SingleRace.fng input=- race=1 go=1 done=0 pursuit=0 -> 1 pause=0' 'LOG status_race: GO' \
  'input=Pause_Main.fng race=1 go=1 done=0 pursuit=0 -> 0 pause=1' 'LOG status_race: paused' \
  'hud=HUD_SingleRace.fng input=- race=1 go=1 done=0 pursuit=0 -> 1 pause=0' 'LOG status_race: resumed' \
  'done=1 pursuit=0 -> 0' 'input=InGamePhotoMaster.fng' 'LOG status_race: results') || fail "missing, or out of order: $miss"
# nothing drives between the pause menu's line and the script's word on it
awk '/input=Pause_Main.fng .*pause=1/{p=1} /LOG status_race: paused/{p=0} p && /\[drive\].*-> [1-9]/' "$LOG" | grep -q . &&   fail "driving with the pause menu up: $(awk '/input=Pause_Main.fng .*pause=1/{p=1} /LOG status_race: paused/{p=0} p && /\[drive\].*-> [1-9]/' "$LOG" | head -1)"
# the winner's screen and the finish before it: no driving once the player has finished
grep -E '\[drive\].* done=1 .*-> [1-9]' "$LOG" | grep -q . && fail "driving after the finish: $(grep -m1 -E '\[drive\].* done=1 .*-> [1-9]' "$LOG")"
grep -qF 'app: quit requested (autotest close) -> WM_CLOSE to the game' "$LOG" || fail "no WM_CLOSE sent"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
echo "drive 9 at the countdown, 1 from GO, 0 with pause 1 in the pause menu, 1 again, 0 on the winner's screen; WM_CLOSE"
