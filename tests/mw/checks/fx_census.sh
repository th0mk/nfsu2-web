#!/bin/bash
# Suite check for "fx_census" (tests/mw/scripts/fx_census.txt, TRACKER M5-15..M5-20): the module under test is named
# (L-020), the race came to GO, the recorder opened its three windows (at GO, after 300 frames and at speed) and
# the game left by WM_CLOSE.  web/tools/fx-census.mjs reads the recording (the save folder's dxrec.txt).
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, ' "$LOG" || fail "no module line naming its revision"
grep -qF 'fx_census: GO' "$LOG" || fail "the race did not reach GO"
n=$(grep -cE '^\[dxrec\] Presents [0-9]+\.\.[0-9]+$' "$LOG")
[ "$n" -eq 3 ] || fail "the recorder opened $n windows, not 3 (NFSU2_DXREC=/save/dxrec.txt,999999999 set?)"
grep -qF 'fx_census: done' "$LOG" || fail "the windows did not all pass"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "the race reached GO, the recorder wrote 3 windows and the game left by WM_CLOSE"
