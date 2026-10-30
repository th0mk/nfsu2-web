#!/bin/bash
# Suite check for "breaker_ai_taps" (tests/mw/scripts/breaker_ai_taps.txt, TRACKER M4-19, M4-47 (a)): the module under test
# is named (L-020); the race came to GO and the AI let go; of the twelve G taps after it, at least one became a
# GAMEBREAKER action the InputPlayer read (the [mwbreaker] "GAMEBREAKER action" lines of runtime/mw/career_mw.c); the
# game left by WM_CLOSE.  Prints the frame of each action after the AI let go, and the queue's input lines.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'LOG breaker_ai_taps: the AI let go' "$LOG" || fail "the AI never let go"
grep -q 'LOG breaker_ai_taps: done' "$LOG" || fail "no 'breaker_ai_taps: done' marker"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
go=$(grep -m1 'LOG breaker_ai_taps: the AI let go' "$LOG" | grep -oE 'f=[0-9]+' | cut -c3-)
acts=$(awk -v go="$go" '/\[mwbreaker\] f=[0-9]+ GAMEBREAKER action: the case/ { split($2, a, "="); if (a[2] + 0 >= go) printf " %d", a[2] - go }' "$LOG")
inp=$(awk -v go="$go" '/\[mwbreaker\] f=[0-9]+ input:/ { split($2, a, "="); if (a[2] + 0 >= go - 700) printf " | %s", substr($0, 12, 90) }' "$LOG")
[ -n "$acts" ] || fail "no GAMEBREAKER action after the AI let go (12 taps);$inp"
echo "GAMEBREAKER actions at frames after the AI let go:$acts;$inp"
