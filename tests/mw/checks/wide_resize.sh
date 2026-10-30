#!/bin/bash
# Suite check for "wide_resize" (tests/mw/scripts/wide_resize.txt, TRACKER M7-03, M7-04): the module under test is
# named (L-020); the device was made again at each new canvas shape (com.c's "device resolution" line after the first,
# from the game's own Reset) and the game was told each new shape (runtime/mw/widescreen_mw.c's "ws: game/device
# aspect" lines); the race reached GO and the script its end; with the renderer (the play page) the pictures before and
# after each change were written; no stall; the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^([0-9.]+ )?module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q 'LOG wide_resize: done' "$LOG" || fail "the script did not end"
devs=$(grep -oE 'com: device resolution [0-9]+x[0-9]+ -> [0-9]+x[0-9]+' "$LOG" | sed 's/.*-> //' | uniq | tr '\n' ' ')
[ "$(echo $devs | wc -w)" -ge 3 ] || fail "the device took $(echo $devs | wc -w) sizes ($devs), not 3: the canvas changes did not reach it"
n=$(grep -cE 'ws: game/device aspect ' "$LOG"); [ "$n" -ge 3 ] || fail "the game was told $n shapes, not 3"
if grep -q '\[gpu\] renderer attached' "$LOG"; then
  for p in menu_before menu_after race_before race_after; do
    grep -qE "\[gpu\] dump (.*_)?nfsu2_g[0-9]+_$p\.png " "$LOG" || fail "no picture of $p"
  done
  pics=", 4 pictures"
fi
grep -qF '[watchdog] no Present' "$LOG" && fail "a stall: $(grep -m1 -F '[watchdog] no Present' "$LOG")"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "the device $devs$pics; the game left by WM_CLOSE"
