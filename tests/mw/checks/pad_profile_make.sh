#!/bin/bash
# Suite check for "pad_profile_make" (tests/mw/scripts/pad_profile_make.txt, TRACKER M6-37): the module under test is
# named (L-020); every wait in time; runtime/mw/pad_mw.c wrote its layout at the input set-up (NFSU2_PAD_LOG=1); the
# 22 pokes of each alias ran, then its save file was written (FILELIST=1), PADA's before PADB's; the game left by
# WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -qE '^\[autotest\] f=[0-9]+ .* TIMEOUT' "$LOG" && fail "a wait of the script ran out: $(grep -m1 -E 'TIMEOUT' "$LOG")"
grep -qE 'poke 0x[0-9a-f]+: no such address' "$LOG" && fail "a poke found no memory"
n=$(grep -cE '^\[autotest\] f=[0-9]+ poke \[0x0091f[0-9a-f]{3}\] <- ' "$LOG")
[ "$n" = 44 ] || fail "$n pokes of the binding table, want 44"
D='"C:\NFSMW\NFS Most Wanted'
miss=$(inorder "[pad] Most Wanted's default pad layout (the input set-up)" \
  "LOG pad_profile_make: the game's layout for PADA" 'poke [0x0091f7e4] <- 0x00000007' \
  "[write] CreateFileA(CREATE_ALWAYS, 0x40000000) $D/PADA/PADA\"" 'LOG pad_profile_make: PADA saved' \
  "LOG pad_profile_make: the game's layout with the gas on Y for PADB" 'poke [0x0091f7e4] <- 0x00000007' \
  "[write] CreateFileA(CREATE_ALWAYS, 0x40000000) $D/PADB/PADB\"" 'LOG pad_profile_make: PADB saved') \
  || fail "missing, or out of order: $miss"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "PADA saved with the game's own pad layout and PADB with the gas on Y as well, the pad plugged in"
