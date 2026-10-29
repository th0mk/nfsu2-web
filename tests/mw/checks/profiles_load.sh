#!/bin/bash
# Suite check for "profiles_load" (tests/mw/scripts/profiles_load.txt, TRACKER M3-20), a start seeded with
# profiles_make's saves: the module under test is named (L-020); the start's Load list loaded ALPHA (its save read)
# and a save from the Alias Manager wrote ALPHA (so ALPHA was the current alias); the Alias Manager's Load loaded
# BRAVO and a save wrote BRAVO; Delete removed ALPHA's save and folder; all in that order (the FILELIST=1 lines of
# runtime/fileio.c); and the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
# the fixed strings given must occur in the log in this order, each on a later line; prints the first one missing
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'waitfng .* TIMEOUT\|tapuntil .* TIMEOUT' "$LOG" && fail "a screen did not come up: $(grep -m1 'TIMEOUT' "$LOG")"
D='"C:\NFSMW\NFS Most Wanted'
grep -qF "[file] FindFirstFileA($D/ALPHA/ALPHA\") -> found" "$LOG" || fail "the start's scan did not find the seeded ALPHA"
grep -qF "[file] FindFirstFileA($D/BRAVO/BRAVO\") -> found" "$LOG" || fail "the start's scan did not find the seeded BRAVO"
miss=$(inorder 'waitfng MC_List.fng satisfied' "[file] CreateFileA($D/ALPHA/ALPHA\") -> " 'LOG profiles_load: ALPHA loaded' \
  "$D/ALPHA/ALPHA\" -> \"/save/" 'LOG profiles_load: ALPHA saved' \
  "[file] CreateFileA($D/BRAVO/BRAVO\") -> " 'LOG profiles_load: BRAVO loaded' \
  "$D/BRAVO/BRAVO\" -> \"/save/" 'LOG profiles_load: BRAVO saved' \
  "[write] DeleteFileA $D/ALPHA/ALPHA\"" "[file] RemoveDirectoryA($D/ALPHA\") -> 1" 'LOG profiles_load: ALPHA deleted') ||
  fail "missing, or out of order: $miss"
# the saves were writes (the lines between the loads and the "saved" marks are [write] lines)
awk '/LOG profiles_load: ALPHA loaded/ { a = 1 } /LOG profiles_load: ALPHA saved/ { a = 0 }
     /LOG profiles_load: BRAVO loaded/ { b = 1 } /LOG profiles_load: BRAVO saved/ { b = 0 }
     a && /^\[write\] .*NFS Most Wanted\/ALPHA\/ALPHA"/ { wa = 1 } b && /^\[write\] .*NFS Most Wanted\/BRAVO\/BRAVO"/ { wb = 1 }
     END { exit !(wa && wb) }' "$LOG" || fail "a save from the Alias Manager wrote nothing"
grep -qF 'app: quit requested (autotest close) -> WM_CLOSE to the game' "$LOG" || fail "no WM_CLOSE sent"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "after a restart: ALPHA loaded and saved, BRAVO loaded and saved, ALPHA deleted; the game left by WM_CLOSE"
