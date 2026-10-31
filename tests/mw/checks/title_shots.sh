#!/bin/bash
# Suite check for "title_shots" (tests/mw/scripts/title_shots.txt, TRACKER M7-39, M7-42): the module under test is
# named (L-020); the title came, the one the screen's shape wants (the widescreen one on a 16:9 or wider screen, the
# 4:3 one on another, or on any when widescreen_mw.c's line says NFSMW_WIDE_SPLASH=0 asked for it); with the renderer
# (the play page) its picture was written; no stall; the game left by WM_CLOSE.
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
# which title: the HUD's factor (4:3 / the screen's aspect) when the title came, and the wide title's construction
# answered (widescreen_mw.c's report, title=N: the wide title's constructions); the wide one belongs to a 16:9 or
# wider screen (no aspect line: the screen is the game's own 4:3, factor 1)
hk=$(awk 'BEGIN { h = "1.0000" } /ws: game\/device aspect [0-9.]+, HUD /{ h = $0; sub(/.*, HUD /, "", h); sub(/:.*/, "", h) }
          /waitfng MW_LS_Splash.fng\|WS_MW_LS_Splash.fng satisfied/{ print h; exit }' "$LOG")
[ -n "$hk" ] || fail "the title's wait never ended"
built=$(grep -E 'ws: mw hooks ' "$LOG" | tail -1 | sed -nE 's/.* title=([0-9]+)[/ ].*/\1/p')
[ -n "$built" ] || fail "no widescreen report with the title's count"
want=4:3; grep -q 'the wide title from 16:9' "$LOG" && awk -v h="$hk" 'BEGIN { exit !(h < 0.76) }' && want=wide
got=4:3; [ "$built" -ge 1 ] && got=wide
[ "$want" = "$got" ] || fail "the $got title came on a screen that wants the $want one (HUD factor $hk)"
echo "the $got title (HUD factor $hk); the game left by WM_CLOSE"
