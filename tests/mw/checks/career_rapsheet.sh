#!/bin/bash
# Suite check for "career_rapsheet" (tests/mw/scripts/career_rapsheet.txt, TRACKER M4-44), a probe run with
# NFSU2_FNG_LOG=3: the module is named; no wait ran out; the Rap Sheet's main screen came and the walk reached its end.
# Prints the Rap Sheet screens that came (by package) and how often.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q 'LOG career_rapsheet: main' "$LOG" || fail "the Rap Sheet's main screen did not come"
grep -q 'LOG career_rapsheet: done' "$LOG" || fail "no 'career_rapsheet: done' marker"
s=$(grep -oE '^\[fng\] f=[0-9]+ screen RapSheet[A-Za-z_]*\.fng' "$LOG" | awk '{print $4}' | sort | uniq -c | awk '{printf "%s%s x%s", (NR>1?", ":""), $2, $1}')
echo "the Rap Sheet's screens: ${s:-none}"
