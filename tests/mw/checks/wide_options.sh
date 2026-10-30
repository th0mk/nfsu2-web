#!/bin/bash
# Suite check for "wide_options" (tests/mw/scripts/wide_options.txt, TRACKER M7-04): the module under test is named
# (L-020); the Video options came up; the game looked up the resolution rows' labels and got ours
# (runtime/mw/widescreen_mw.c's exit report: labels > 0); with the renderer (the play page) the rows' pictures were
# written; the game left by WM_CLOSE.  What each label says is read from the pictures, outside this check.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^([0-9.]+ )?module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -qE 'tapuntil OptionsPCDisplay\.fng(\|OptionsPCDisplay\.fng)? satisfied' "$LOG" || fail "the Video options never came up"
grep -q 'LOG wide_options: done' "$LOG" || fail "the script did not end"
n=$(grep -E 'ws: mw hooks ' "$LOG" | tail -1 | sed -nE 's/.* labels=([0-9]+) .*/\1/p')
[ -n "$n" ] && [ "$n" -gt 0 ] || fail "the resolution labels were never ours (labels=${n:-none})"
if grep -q '\[gpu\] renderer attached' "$LOG"; then
  [ "$(grep -cE '\[gpu\] dump (.*_)?nfsu2_g[0-9]+_video_row\.png ' "$LOG")" -ge 12 ] || fail "not every row's picture was written"
  pics=", 6 rows' pictures"
fi
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "the resolution labels looked up $n times$pics; the game left by WM_CLOSE"
