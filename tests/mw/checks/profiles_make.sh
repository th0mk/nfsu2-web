#!/bin/bash
# Suite check for "profiles_make" (tests/mw/scripts/profiles_make.txt, TRACKER M3-20): the module under test is
# named (L-020), ALPHA was made and saved at the first start, the Alias Manager came up, BRAVO was made and saved
# after it (each alias's folder and save file in the save tree, the FILELIST=1 lines of runtime/fileio.c), and the
# game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
# the fixed strings given must occur in the log in this order, each on a later line; prints the first one missing
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
# any wait of the script that ran out (waitfng, tapuntil, waitmem, waitexpr ...), not only a screen's
grep -qE '^\[autotest\] f=[0-9]+ .* TIMEOUT' "$LOG" && fail "a wait of the script ran out: $(grep -m1 -E '^\[autotest\] f=[0-9]+ .* TIMEOUT' "$LOG")"
D='"C:\NFSMW\NFS Most Wanted'
miss=$(inorder "[write] CreateDirectoryA $D/ALPHA\"" "[write] CreateFileA(CREATE_ALWAYS, 0x40000000) $D/ALPHA/ALPHA\"" \
  'LOG profiles_make: ALPHA saved' 'waitfng MC_ProfileManager.fng' \
  "[write] CreateDirectoryA $D/BRAVO\"" "[write] CreateFileA(CREATE_ALWAYS, 0x40000000) $D/BRAVO/BRAVO\"" \
  'LOG profiles_make: BRAVO saved') || fail "missing, or out of order: $miss"
grep -qF 'app: quit requested (autotest close) -> WM_CLOSE to the game' "$LOG" || fail "no WM_CLOSE sent"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "ALPHA made and saved at the first start, BRAVO made and saved from the Alias Manager; the game left by WM_CLOSE"
