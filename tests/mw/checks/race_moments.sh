#!/bin/bash
# Suite check for "race_moments" (tests/mw/scripts/race_moments.txt, TRACKER M5-32, M5-33): the module under test is
# named (L-020); the race reached GO and every moment after it (the race timer 0.03, 0.18, 0.30, 1, 3, 8 and 12 s past
# its value at GO), each wait in time, in rising frames; with the renderer (the play page: its "[gpu] renderer
# attached" line; the suite's page has none)
# each moment's picture was written, under its own frame; no stall report; the game left by WM_CLOSE.  Prints the
# frame and the timer value of each moment.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^([0-9.]+ )?module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
go=$(grep -m1 -E '\[autotest\] f=[0-9]+ mark go = ' "$LOG")
[ -n "$go" ] && grep -q 'LOG race_moments: GO' "$LOG" || fail "the race never reached GO"
gof=$(echo "$go" | sed -E 's/.*f=([0-9]+) .*/\1/'); gov=$(echo "$go" | sed -E 's/.* = //')
m=$(grep -E '\[autotest\] f=[0-9]+ waitexpr satisfied after [0-9]+ frames: f:0x009142dc >= ' "$LOG")
[ "$(echo "$m" | grep -c .)" = 7 ] || fail "$(echo "$m" | grep -c .) of the 7 moments after GO reached"
grep -q 'LOG race_moments: done' "$LOG" || fail "the script did not end"
# each moment: its frame and the timer past GO; frames rise
out=$(echo "$m" | awk -v gf="$gof" -v gv="$gov" '{ f = $0; sub(/.*f=/, "", f); f += 0; v = $0; sub(/.*\(value /, "", v); sub(/\).*/, "", v)
  if (f <= last) bad = 1; last = f; printf " f%d +%.3f s", f, v - gv } END { exit bad }') || fail "the moments' frames do not rise:$out"
if grep -q '\[gpu\] renderer attached' "$LOG"; then
  for t in t0000 t0003 t0018 t0030 t0100 t0300 t0800 t1200; do
    grep -qE "\[gpu\] dump (.*_)?nfsu2_g[0-9]+_$t\.png " "$LOG" || fail "no picture of $t"
  done
  pics=", 8 pictures"
fi
grep -qF '[watchdog] no Present' "$LOG" && fail "a stall: $(grep -m1 -F '[watchdog] no Present' "$LOG")"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "GO at f$gof (timer $gov), the moments:$out$pics; the game left by WM_CLOSE"
