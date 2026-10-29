#!/bin/bash
# Suite check for "career_new" (tests/mw/scripts/career_new.txt, TRACKER M4-03), a start seeded with an alias that
# has no career (profiles_make's saves): the module under test is named (L-020); the start's Load list read the
# seeded ALPHA (FILELIST=1's [file] lines), the main menu came, and Career showed the career menu of a new alias,
# on Start Career, in that order; nothing was written to ALPHA; the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
# the fixed strings given must occur in the log in this order, each on a later line; prints the first one missing
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
D='"C:\NFSMW\NFS Most Wanted'
miss=$(inorder "[file] CreateFileA($D/ALPHA/ALPHA\") -> " 'LOG career_new: ALPHA loaded' \
  'waitfng MainMenu_Sub.fng:6005281e@4b4e388a satisfied' 'LOG career_new: Start Career') ||
  fail "missing, or out of order: $miss"
grep -qF "[write] CreateFileA(CREATE_ALWAYS, 0x40000000) $D/ALPHA/ALPHA\"" "$LOG" && fail "ALPHA was written"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "the new-alias seed: ALPHA loaded to the main menu, Career offers Start Career; nothing written; the game left by WM_CLOSE"
