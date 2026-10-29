#!/bin/bash
# Suite check for "profiles_gone" (tests/mw/scripts/profiles_gone.txt, TRACKER M3-20), a start seeded with
# profiles_load's saves, where ALPHA was deleted: the module under test is named (L-020); no file line names ALPHA
# (no scan finds it, nothing opens it: the FILELIST=1 lines of runtime/fileio.c; the seed holds the folders the
# delete left empty too, web/tools/run-suite.mjs --save-out and save-seed.mjs, so a folder of ALPHA left behind
# would be found here); the start's scans found BRAVO
# alone, so it loaded BRAVO with no Load list (the list comes up for two or more); and the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
# the fixed strings given must occur in the log in this order, each on a later line; prints the first one missing
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'waitfng .* TIMEOUT\|tapuntil .* TIMEOUT' "$LOG" && fail "a screen did not come up: $(grep -m1 'TIMEOUT' "$LOG")"
grep -qF '[suite] save seed: ' "$LOG" || fail "the run was not seeded with a save tree"
grep -qE '\[(file|write)\].*ALPHA' "$LOG" && fail "the deleted ALPHA came back: $(grep -m1 -E '\[(file|write)\].*ALPHA' "$LOG")"
D='"C:\NFSMW\NFS Most Wanted'
grep -qF '[file] Find -> "BRAVO" attr=0x10' "$LOG" || fail "no scan found BRAVO's folder"
grep -qF "[file] FindFirstFileA($D/BRAVO/BRAVO\") -> found" "$LOG" || fail "the start's scan did not find BRAVO's save"
grep -qE '^\[fng\] f=[0-9]+ screen MC_List\.fng' "$LOG" && fail "the start asked which alias to load: more than one is left"
miss=$(inorder 'tapuntil MC_Main.fng satisfied' "[file] CreateFileA($D/BRAVO/BRAVO\") -> " 'waitfng MainMenu.fng' \
  'LOG profiles_gone: BRAVO loaded') || fail "missing, or out of order: $miss"
grep -qF 'app: quit requested (autotest close) -> WM_CLOSE to the game' "$LOG" || fail "no WM_CLOSE sent"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "after the restart nothing names the deleted ALPHA; BRAVO found and loaded; the game left by WM_CLOSE"
