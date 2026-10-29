#!/bin/bash
# Suite check for "pursuit_start" (tests/mw/scripts/pursuit_start.txt, TRACKER M4-16): the module under test is named
# (L-020); every wait in time; free roam came, Game.ForcePursuitStart was called, the pursuit object came on the
# player's car and later went, in that order; the game left by WM_CLOSE.  Prints how long the pursuit lasted.
LOG="$1"
fail(){ echo "$1"; exit 1; }
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
miss=$(inorder 'call 0x0060aac0 -> ' 'pursuit wait on: satisfied' 'LOG pursuit_start: started' \
  'pursuit wait off: satisfied' 'LOG pursuit_start: ended') || fail "missing, or out of order: $miss"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
awk '/pursuit wait on: satisfied/ { split($2, a, "="); s = a[2] } /pursuit wait off: satisfied/ { split($2, a, "="); e = a[2] }
     END { printf "the pursuit lasted %d frames\n", e - s }' "$LOG"
