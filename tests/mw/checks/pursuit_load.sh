#!/bin/bash
# Suite check for "pursuit_load" (tests/mw/scripts/pursuit_load.txt, TRACKER M4-18): the module under test is named
# (L-020); no wait ran out; the rap sheet was logged at the safehouse; the game left by WM_CLOSE.  With PR_SAVED_LOG
# naming the pursuit_rap run whose save tree seeded this one, its last rap sheet (logged after the save) must equal
# this one line for line: the ten values, the best pursuit and the ranks, the cash, markers and infractions, and each
# career car's record with its impound strikes.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q 'LOG pursuit_load: loaded' "$LOG" || fail "no 'pursuit_load: loaded' marker"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
# the last rap sheet of a log: its first line and the indented ones after it, without frame numbers and the reason
lastrap(){ grep -E '^\[mwrap\] ' "$1" | awk '/ rapsheet \(/ { n = 0; delete b } { b[n++] = $0 } END { for (i = 0; i < n; i++) print b[i] }' |
  sed -E 's/ f=[0-9]+ / /; s/rapsheet \([^)]*\)/rapsheet/'; }
now=$(lastrap "$LOG")
[ -n "$now" ] || fail "no rap sheet logged"
if [ -n "$PR_SAVED_LOG" ]; then
  [ -f "$PR_SAVED_LOG" ] || fail "PR_SAVED_LOG $PR_SAVED_LOG is not a file"
  was=$(lastrap "$PR_SAVED_LOG")
  [ "$now" = "$was" ] || fail "the rap sheet differs from the saved one: $(diff <(echo "$was") <(echo "$now") | grep '^[<>]' | head -4 | tr '\n' ' ')"
  echo "the rap sheet equals the saved one ($(echo "$now" | wc -l) lines):$(echo "$now" | head -1 | sed 's/.*rapsheet://')"
else
  echo "rap sheet:$(echo "$now" | head -1 | sed 's/.*rapsheet://')"
fi
