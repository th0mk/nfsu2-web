#!/bin/bash
# Suite check for "title_shots" (tests/mw/scripts/title_shots.txt, TRACKER M7-39): the module under test is named
# (L-020); the title came (the 4:3 one, or the widescreen one when widescreen_mw.c's line says NFSMW_WIDE_SPLASH is on
# and the screen is 16:9 or wider); with the renderer (the play page) its picture was written; no stall; the game
# left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^([0-9.]+ )?module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q 'LOG title_shots: done' "$LOG" || fail "the script did not end"
pkg=$(grep -oE 'waitfng MW_LS_Splash.fng\|WS_MW_LS_Splash.fng satisfied' "$LOG" | head -1)
[ -n "$pkg" ] || fail "the title never took input"
if grep -q '\[gpu\] renderer attached' "$LOG"; then
  grep -qE "\[gpu\] dump (.*_)?nfsu2_g[0-9]+_title\.png " "$LOG" || fail "no picture of the title"
fi
grep -qF '[watchdog] no Present' "$LOG" && fail "a stall: $(grep -m1 -F '[watchdog] no Present' "$LOG")"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "the title ($(grep -q 'the wide title from 16:9' "$LOG" && echo 'wide title asked for' || echo '4:3 title')); the game left by WM_CLOSE"
