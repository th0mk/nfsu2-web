#!/bin/bash
# Suite check for "hfr_laps" (tests/mw/scripts/hfr_laps.txt, TRACKER M7-32): the module under test is named (L-020);
# the race reached GO and the player's finish with every wait in time and every expect met; racestatus logged the
# player's race as finished; no stall report.  Prints the frame-rate report's mode and world steps and the racers'
# race times.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -qF '[expect] FAIL' "$LOG" && fail "$(grep -m1 -F '[expect] FAIL' "$LOG")"
grep -q 'LOG hfr_laps: GO$' "$LOG" || fail "no GO"
grep -q 'LOG hfr_laps: finished$' "$LOG" || fail "no finish"
grep -qF '[watchdog] no Present' "$LOG" && fail "a stall: $(grep -m1 -F '[watchdog] no Present' "$LOG")"
rs=$(grep -F '[mwrace]' "$LOG" | grep -F 'racestatus' | tail -1)
case "$rs" in *'#0 '*finished*) ;; *) fail "racestatus has no finished player: $rs" ;; esac
mode=$(grep -m1 -F '=== frame rate (Most Wanted)' "$LOG" | sed 's/^.*: mode/mode/; s/ ===$//')
echo "PASS hfr_laps: ${mode:-no frame-rate report}; ${rs#*player }"
