#!/bin/bash
# Suite check for "pad_race" (tests/mw/scripts/pad_race.txt, TRACKER M6-16, M6-37): the module under test is named
# (L-020); the pad's X drove the car past 20 mph, RT brought the pause menu up (status_mw.c's [drive] line with
# "pause" 1), Start did not (no pause line between its press and the next word); the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -qE '^\[autotest\] f=[0-9]+ .* TIMEOUT' "$LOG" && fail "a wait of the script ran out: $(grep -m1 -E '^\[autotest\] f=[0-9]+ .* TIMEOUT' "$LOG")"
miss=$(inorder 'LOG pad_race: GO' 'LOG pad_race: gas by X' 'input=Pause_Main.fng race=1 go=1 done=0 pursuit=0 -> 0 pause=1' \
  'LOG pad_race: paused by RT' 'LOG pad_race: resumed' 'LOG pad_race: start pressed' 'LOG pad_race: results') || fail "missing, or out of order: $miss"
awk '/LOG pad_race: resumed/{p=1} /LOG pad_race: start pressed/{p=0} p && /\[drive\].*pause=1/' "$LOG" | grep -q . && fail "Start paused the race"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
echo "the pad's X drove past 20 mph, RT paused, Start did not; WM_CLOSE"
