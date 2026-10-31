#!/bin/bash
# Suite check for "hfr_pursuit" (tests/mw/scripts/hfr_pursuit.txt, TRACKER M7-32): the module under test is named
# (L-020); the pursuit started and the game ended it (evaded or busted) with every wait in time and every expect
# met; no stall report.  Prints the frame-rate mode, how the pursuit ended, its own time and the most cops it had.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -qF '[expect] FAIL' "$LOG" && fail "$(grep -m1 -F '[expect] FAIL' "$LOG")"
grep -q 'LOG hfr_pursuit: started$' "$LOG" || fail "no pursuit"
grep -q 'LOG hfr_pursuit: ended$' "$LOG" || fail "the pursuit did not end"
grep -qF '[watchdog] no Present' "$LOG" && fail "a stall: $(grep -m1 -F '[watchdog] no Present' "$LOG")"
how=$(grep -oE '\[mwpursuit\] f=[0-9]+ (evaded|busted)' "$LOG" | head -1 | awk '{print $3}')
[ -n "$how" ] || fail "no evaded or busted line"
t=$(grep -oE '\[mwpursuit\] f=[0-9]+ heat [0-9.]+ pursuit [0-9a-f]{8} time [0-9.]+' "$LOG" | grep -v 'pursuit 00000000' | tail -1 | awk '{print $NF}')
cops=$(grep -oE ' cops [0-9]+' "$LOG" | awk '{print $2}' | sort -n | tail -1)
mode=$(grep -m1 -F '=== frame rate (Most Wanted)' "$LOG" | sed 's/^.*: mode/mode/; s/ ===$//')
echo "PASS hfr_pursuit: ${mode:-no frame-rate report}; $how after ${t:-?} s of pursuit time, at most ${cops:-0} cops"
