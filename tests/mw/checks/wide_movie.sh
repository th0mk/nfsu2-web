#!/bin/bash
# Suite check for "wide_movie" (tests/mw/scripts/wide_movie.txt, TRACKER M7-05): the module under test is named
# (L-020); the movie framing the run asked for was read (runtime/mw/widescreen_mw.c's "ws: mw" line); the attract
# movie came up and the script ended; with the renderer (the play page) its picture was written; the game left by
# WM_CLOSE.  The picture's rectangle is measured from the dump, outside this check.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^([0-9.]+ )?module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
mode=$(grep -m1 -oE 'ws: mw hud=[a-z0-9:]+ movie=[a-z]+' "$LOG") || fail "no widescreen options line"
grep -q 'LOG wide_movie: done' "$LOG" || fail "the script did not end"
asp=$(grep -E 'ws: game/device aspect ' "$LOG" | tail -1 | sed -E 's/.*ws: game\/device aspect //')
if grep -q '\[gpu\] renderer attached' "$LOG"; then
  grep -qE '\[gpu\] dump (.*_)?nfsu2_g[0-9]+_movie\.png ' "$LOG" || fail "no picture of the movie"
  pics=", its picture"
fi
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "${mode#ws: mw }, aspect ${asp:-1 (4:3)}$pics; the game left by WM_CLOSE"
