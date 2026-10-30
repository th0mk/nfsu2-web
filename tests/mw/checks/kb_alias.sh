#!/bin/bash
# Suite check for "kb_alias" (tests/mw/scripts/kb_alias.txt, TRACKER M3-11): the module under test is named (L-020),
# the alias typed by keyboard was saved as ROTEST (its folder and file in the save tree, the name as typed, Shift
# included), the Alias Manager and its Load list came up, the list's scan of the save folder found ROTEST (the
# FILELIST=1 lines of runtime/fileio.c), and the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
# any wait of the script that ran out (waitfng, tapuntil, waitmem, waitexpr ...), not only a screen's
grep -qE '^\[autotest\] f=[0-9]+ .* TIMEOUT' "$LOG" && fail "a wait of the script ran out: $(grep -m1 -E '^\[autotest\] f=[0-9]+ .* TIMEOUT' "$LOG")"
grep -qF '[write] CreateDirectoryA "C:\NFSMW\NFS Most Wanted/ROTEST"' "$LOG" || fail "no save folder ROTEST: the name typed was $(grep -m1 -oE 'CreateDirectoryA "C:.NFSMW.NFS Most Wanted/[^"]*"' "$LOG" || echo nothing)"
grep -qE '^\[write\] CreateFileA\(CREATE_ALWAYS, [^)]*\) "C:\\NFSMW\\NFS Most Wanted/ROTEST/ROTEST"' "$LOG" || fail "the save ROTEST/ROTEST was not written"
for s in MainMenu.fng MC_ProfileManager.fng MC_List.fng; do
  grep -qE "^\[autotest\] f=[0-9]+ waitfng $s[^ ]* satisfied" "$LOG" || fail "$s did not come up"
done
# the list's folder scan, after the list was asked for: the alias folder and its save file
awk '
  /waitfng MC_ProfileManager\.fng[^ ]* satisfied/ { pm = 1 }
  pm && /\[file\] Find -> "ROTEST" attr=0x10/ { dir = 1 }
  pm && dir && /\[file\] FindFirstFileA\("C:\\NFSMW\\NFS Most Wanted\/ROTEST\/ROTEST"\) -> found/ { ok = 1; exit }
  END { exit !ok }' "$LOG" || fail "the Load list did not find ROTEST in the save folder"
grep -qF 'app: quit requested (autotest close) -> WM_CLOSE to the game' "$LOG" || fail "no WM_CLOSE sent"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "ROTEST typed, saved as ROTEST/ROTEST and found by the Alias Manager's Load list; the game left by WM_CLOSE"
