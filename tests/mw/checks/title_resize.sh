#!/bin/bash
# Suite check for "title_resize" (tests/mw/scripts/title_resize.txt, TRACKER M7-42): the module under test is named
# (L-020); at each change of the screen's shape across 16:9 the title was made again for the new shape at once
# (runtime/mw/widescreen_mw.c's "the title made again" lines: the 4:3 one after the change to 4:3, then the wide one),
# not through the title's own round (no attract movie after the first change); with the renderer (the play page)
# both pictures were written; no stall; the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^([0-9.]+ )?module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q 'LOG title_resize: done' "$LOG" || fail "the script did not end"
remade=$(grep -oE 'ws: f=[0-9]+ the title made again for the screen.s shape: [A-Za-z_]+\.fng' "$LOG" | sed 's/.*: //' | tr '\n' ' ')
[ "$remade" = "MW_LS_Splash.fng WS_MW_LS_Splash.fng " ] || fail "the title was made again as: '${remade% }', not the 4:3 one and then the wide one"
awk '/LOG title_resize: resize 1/{ on = 1 } on && /\[fng\] f=[0-9]+ screen MW_LS_AttractFMV.fng/{ bad = 1 } END { exit bad }' "$LOG" ||
  fail "the attract movie came between the changes: the title's own round, not the change"
if grep -q '\[gpu\] renderer attached' "$LOG"; then
  for p in title_43 title_169; do
    grep -qE "\[gpu\] dump (.*_)?nfsu2_g[0-9]+_$p\.png " "$LOG" || fail "no picture of $p"
  done
  pics=", 2 pictures"
fi
grep -qF '[watchdog] no Present' "$LOG" && fail "a stall: $(grep -m1 -F '[watchdog] no Present' "$LOG")"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "the title made again as ${remade% }$pics; the game left by WM_CLOSE"
