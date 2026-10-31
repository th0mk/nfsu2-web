#!/bin/bash
# Suite check for "wide_views" (tests/mw/scripts/wide_views.txt, TRACKER M7-01): the module under test is named
# (L-020); the race reached GO and the script its end; with the renderer (the play page) each view's picture was
# written (far, near, bumper, hood, look back); no stall; the game left by WM_CLOSE.  Whether each shows the car or
# the hood is read from the pictures.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^([0-9.]+ )?module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q 'LOG wide_views: GO' "$LOG" || fail "the race never reached GO"
grep -q 'LOG wide_views: done' "$LOG" || fail "the script did not end"
if grep -q '\[gpu\] renderer attached' "$LOG"; then
  for p in far near bumper hood lookback; do
    grep -qE "\[gpu\] dump (.*_)?nfsu2_g[0-9]+_view_$p\.png " "$LOG" || fail "no picture of the $p view"
  done
  pics=", 5 views"
fi
grep -qF '[watchdog] no Present' "$LOG" && fail "a stall: $(grep -m1 -F '[watchdog] no Present' "$LOG")"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "$(grep -E 'ws: mw hooks ' "$LOG" | tail -1 | sed -E 's/.*\((k=[^,]*),.*/\1/')$pics; the game left by WM_CLOSE"
