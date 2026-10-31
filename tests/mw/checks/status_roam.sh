#!/bin/bash
# Suite check for "status_roam" (tests/mw/scripts/status_roam.txt, TRACKER M6-15): the module under test is named
# (L-020), and runtime/mw/status_mw.c's [drive] lines come in this order: free roam (2), the pause menu (0, "pause"
# 1), free roam again, the chase (2 | 16 = 18), and no chase once it is over; the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -qE '^\[autotest\] f=[0-9]+ .* TIMEOUT' "$LOG" && fail "a wait of the script ran out: $(grep -m1 -E '^\[autotest\] f=[0-9]+ .* TIMEOUT' "$LOG")"
miss=$(inorder 'hud=HUD_SingleRace.fng input=- race=0 go=0 done=0 pursuit=0 -> 2 pause=0' 'LOG status_roam: free roam' \
  'input=Pause_Main.fng race=0 go=0 done=0 pursuit=0 -> 0 pause=1' 'LOG status_roam: paused' \
  'hud=HUD_SingleRace.fng input=- race=0 go=0 done=0 pursuit=0 -> 2 pause=0' 'LOG status_roam: resumed' \
  'pursuit=1 -> 18 pause=0' 'LOG status_roam: chased' 'pursuit=0 -> ' 'LOG status_roam: chase over') || fail "missing, or out of order: $miss"
grep -qF 'app: quit requested (autotest close) -> WM_CLOSE to the game' "$LOG" || fail "no WM_CLOSE sent"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
echo "drive 2 in free roam, 0 with pause 1 in the pause menu, 2 again, 18 in the chase, the chase gone after it; WM_CLOSE"
