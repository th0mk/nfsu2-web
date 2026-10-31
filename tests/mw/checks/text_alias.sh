#!/bin/bash
# Suite check for "text_alias" (tests/mw/scripts/text_alias.txt, TRACKER M6-21): the module under test is named
# (L-020); every wait in time; the alias name's text entry came up with NAME in it (kind 6, at most 7), went empty with
# the Backspaces, read ROTEST once typed, and went down after Accept, in that order; the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -qE '^\[autotest\] f=[0-9]+ .* TIMEOUT' "$LOG" && fail "a wait of the script ran out: $(grep -m1 -E 'TIMEOUT' "$LOG")"
miss=$(inorder "[input] the game's text entry is up" '[input] text entry: kind 6, at most 7, "NAME"' \
  '[input] text entry: kind 6, at most 7, ""' '[input] text entry: kind 6, at most 7, "ROTEST"' 'LOG text_alias: typed ROTEST' \
  "[input] the game's text entry is down" 'LOG text_alias: accepted') || fail "missing, or out of order: $miss"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
echo "the alias entry went up with NAME (kind 6, at most 7), emptied, read ROTEST, and went down after Accept"
