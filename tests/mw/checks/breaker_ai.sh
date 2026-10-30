#!/bin/bash
# Suite check for "breaker_ai" (tests/mw/scripts/breaker_ai.txt, TRACKER M4-47 (a)): the module under test is named
# (L-020); the race came to GO and the AI let go; G switched the Speedbreaker on in time (the wait's answer); the game
# left by WM_CLOSE.  Prints how many frames the press took after the AI let go and what CanUse said meanwhile.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'LOG breaker_ai: the AI let go' "$LOG" || fail "the AI never let go"
grep -q 'breaker wait on: TIMEOUT' "$LOG" && fail "G did not switch it on in 900 frames: $(grep -E '\[mwbreaker\] f=[0-9]+ can' "$LOG" | tail -3 | tr '\n' ' ')"
grep -q 'breaker wait on: satisfied' "$LOG" || fail "no answer from the wait"
grep -q 'LOG breaker_ai: done' "$LOG" || fail "no 'breaker_ai: done' marker"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
awk '/LOG breaker_ai: the AI let go/ { on = 1 } on && /\[mwbreaker\] f=[0-9]+ can/ { printf "%s ", $2 " " $3 " " $4 " " $5 " " $6 }
     /breaker wait on: satisfied/ { print "| " $0; exit }' "$LOG" | sed 's/\[mwbreaker\] //'
