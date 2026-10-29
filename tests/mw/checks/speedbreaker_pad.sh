#!/bin/bash
# Suite check for "speedbreaker_pad" (tests/mw/scripts/speedbreaker_pad.txt, TRACKER M4-19): the module under test
# is named (L-020); every wait in time; the race reached GO; the pad's Y switched the Speedbreaker on, and later off;
# the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
miss=$(inorder 'LOG speedbreaker_pad: GO' 'breaker wait on: satisfied' 'LOG speedbreaker_pad: on by Y' \
  'breaker wait off: satisfied' 'LOG speedbreaker_pad: off by Y') || fail "missing, or out of order: $miss"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
echo "the pad's Y switched the Speedbreaker on and off"
