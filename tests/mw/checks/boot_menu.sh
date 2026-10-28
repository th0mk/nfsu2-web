#!/bin/bash
# Suite check for "boot_menu" (tests/mw/scripts/boot_menu.txt, TRACKER M2-25): the module under test is named
# (L-020), the boot laid out Most Wanted's memory map and ran every static initializer, the main menu came up,
# and the game left by WM_CLOSE through its own ExitProcess(0), not by the frame limit or a forced exit.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
mem=$(grep -m1 -E '^\[mem\] mw: ' "$LOG") || fail "no [mem] line"
for want in 'image 0x00400000-' 'server.dll 0x00d00000-' 'handle values: files 0x00a80000-' 'objects 0x00a90000-' 'no overlap'; do
  case "$mem" in *"$want"*) ;; *) fail "the [mem] line lacks '$want'";; esac
done
grep -qE '^\[init\] 5 of 5 C initializers .* 870 of 870 C\+\+ constructors .*, set$' "$LOG" \
  || fail "the [init] line does not show 5 of 5 C initializers and 870 of 870 C++ constructors"
grep -qE '^\[autotest\] f=[0-9]+ waitfng MainMenu\.fng satisfied' "$LOG" || fail "the main menu did not come up"
grep -qF 'app: quit requested (autotest close) -> WM_CLOSE to the game' "$LOG" || fail "no WM_CLOSE sent"
grep -qF '*** guest ExitProcess(0) - exiting' "$LOG" || fail "the game did not call ExitProcess(0)"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "module, [mem] and [init] as expected; the main menu came up; the game left by WM_CLOSE and ExitProcess(0)"
