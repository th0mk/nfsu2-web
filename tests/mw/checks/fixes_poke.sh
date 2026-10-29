#!/bin/bash
# Suite check for "fixes_poke" (tests/mw/scripts/fixes_poke.txt, TRACKER M4-29): the module under test is named
# (L-020); the front end's cop list was as the script expects; with the fixes (no NFSU2_TEST_* switch) both null
# guards logged their line and nothing was called through a null pointer; the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q '^\[expect\] FAIL' "$LOG" && fail "$(grep -m1 '^\[expect\] FAIL' "$LOG")"
grep -q '^\[fix\] f=[0-9]* Attrib::StringToLowerCaseKey(null)' "$LOG" || fail "no StringToLowerCaseKey(null) guard line"
grep -q '^\[fix\] f=[0-9]* cop unload: entry 0 of the list at 0x92FD8C is null' "$LOG" || fail "no cop-unload guard line"
grep -q 'UNRESOLVED INDIRECT' "$LOG" && fail "a call through a null pointer: $(grep -m1 'UNRESOLVED INDIRECT' "$LOG")"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
echo ok
