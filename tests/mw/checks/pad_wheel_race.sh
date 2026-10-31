#!/bin/bash
# Suite check for "pad_wheel_race" (tests/mw/scripts/pad_wheel_race.txt, TRACKER M6-37, M6-38): the module under test
# is named (L-020); every wait in time; the wheel's gas pedal drove the car past 20 mph without bringing up the pause
# menu (no [drive] line with "pause" 1 between GO and the brake), its brake pedal slowed it below 10 mph, Menu brought
# the pause menu up (status_mw.c's [drive] line with "pause" 1); pad_mw.c wrote its layout where the log has the
# line (NFSU2_PAD_LOG=1); the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -qE '^\[autotest\] f=[0-9]+ .* TIMEOUT|breaker wait [a-z]+: TIMEOUT' "$LOG" && fail "a wait of the script ran out: $(grep -m1 -E 'TIMEOUT' "$LOG")"
if grep -q '^\[pad\] ' "$LOG"; then
  grep -qF "[pad] Most Wanted's default wheel layout" "$LOG" || fail "NFSU2_PAD_LOG is on but no line of pad_mw.c's layout"
fi
miss=$(inorder 'LOG pad_wheel_race: GO' 'breaker wait above: satisfied' 'LOG pad_wheel_race: gas by the pedal' 'breaker wait below: satisfied'   'LOG pad_wheel_race: braked by the pedal' 'input=Pause_Main.fng race=1 go=1 done=0 pursuit=0 -> 0 pause=1'   'LOG pad_wheel_race: paused by Menu' 'LOG pad_wheel_race: resumed' 'LOG pad_wheel_race: results') || fail "missing, or out of order: $miss"
awk '/LOG pad_wheel_race: GO/{p=1} /LOG pad_wheel_race: braked by the pedal/{p=0} p && /\[drive\].*pause=1/' "$LOG" | grep -q . && fail "the gas pedal paused the race"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
echo "the wheel's gas pedal drove past 20 mph and did not pause, its brake pedal braked below 10 mph, Menu paused; WM_CLOSE"
