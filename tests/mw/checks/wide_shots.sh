#!/bin/bash
# Suite check for "wide_shots" (tests/mw/scripts/wide_shots.txt, TRACKER M7-01..M7-06): the module under test is named
# (L-020); the device's shape reached the game (runtime/mw/widescreen_mw.c's "ws: game/device aspect" line, unless the
# canvas is 4:3); the projection's Hor+ (or Vert+) ran for the player's view when the shape is not 4:3; the race
# reached GO and the script its end; with the renderer (the play page) every picture was written; no stall report; the
# game left by WM_CLOSE.  Prints the shape's factors and the hooks' counts.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^([0-9.]+ )?module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q 'LOG wide_shots: GO' "$LOG" || fail "the race never reached GO"
grep -q 'LOG wide_shots: done' "$LOG" || fail "the script did not end"
asp=$(grep -E 'ws: game/device aspect ' "$LOG" | tail -1 | sed -E 's/.*ws: game\/device aspect //')
rep=$(grep -E 'ws: mw hooks ' "$LOG" | tail -1 | sed -E 's/.*ws: mw hooks //')
[ -n "$rep" ] || fail "no widescreen report at the exit"
k=$(echo "$rep" | sed -E 's/.*\(k=([0-9.]+),.*/\1/')
if [ -n "$asp" ] && [ "$k" != "1.0000" ]; then
  grep -qE 'ws: view 1 P(00|11) ' "$LOG" || fail "the player's view was never given the screen's shape (k=$k)"
fi
if grep -q '\[gpu\] renderer attached' "$LOG"; then
  for p in movie title mainmenu carselect race_go race_chase race_bumper; do
    grep -qE "\[gpu\] dump (.*_)?nfsu2_g[0-9]+_$p\.png " "$LOG" || fail "no picture of $p"
  done
  pics=", 7 pictures"
fi
grep -qF '[watchdog] no Present' "$LOG" && fail "a stall: $(grep -m1 -F '[watchdog] no Present' "$LOG")"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "aspect ${asp:-1 (4:3)}; hooks $rep$pics; the game left by WM_CLOSE"
