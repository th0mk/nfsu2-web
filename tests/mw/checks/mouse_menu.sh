#!/bin/bash
# Suite check for "mouse_menu" (tests/mw/scripts/mouse_menu.txt, TRACKER M3-08, M3-32): the module under test is
# named (L-020), every wait of the script ended satisfied (the wheel moved the main menu's highlight one item a
# notch, Career .. Options, and back one), the screens came up in order by mouse clicks (the main menu, its Options
# sub menu, the options screen, twice: as the game runs and with the windowed flag set), the cursor was where the
# clicks needed it (the [expect] lines: the selection frame, then 24 units higher with the flag), and the game left
# by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -qE '^\[autotest\] f=[0-9]+ .* TIMEOUT' "$LOG" && fail "a wait of the script ran out: $(grep -m1 -E '^\[autotest\] f=[0-9]+ .* TIMEOUT' "$LOG")"
grep -qE '^\[expect\] FAIL' "$LOG" && fail "a check of the script failed: $(grep -m1 -E '^\[expect\] FAIL' "$LOG")"
n=$(grep -cE '^\[expect\] PASS' "$LOG"); [ "$n" -eq 6 ] || fail "$n of the script's 6 [expect] lines passed"
dev=$(grep -m1 -oE 'com: device resolution [0-9]+x[0-9]+ -> [0-9]+x[0-9]+' "$LOG") || fail "no device resolution line"
# each screen once, however many of its states the script waited for (as kb_menu.sh)
got=$(grep -oE '^\[autotest\] f=[0-9]+ waitfng [A-Za-z_]+\.fng[^ ]* satisfied' "$LOG" | awk '{ sub(/[:@^'\''].*/, "", $4); print $4 }' | uniq | tr '\n' ' ')
want='MainMenu.fng MainMenu_Sub.fng Options.fng MainMenu_Sub.fng MainMenu.fng MainMenu_Sub.fng Options.fng '
[ "$got" = "$want" ] || fail "the screens came up as '$got', not '$want'"
grep -qF 'LOG mouse_menu: Options highlighted by the wheel' "$LOG" || fail "the wheel did not take the highlight to Options"
grep -qF 'LOG mouse_menu: windowed, the cursor 24 units up' "$LOG" || fail "no windowed walk"
grep -qF 'app: quit requested (autotest close) -> WM_CLOSE to the game' "$LOG" || fail "no WM_CLOSE sent"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "Options picked by wheel and clicks twice (windowed flag 0, then 1: the cursor 24 units up); ${dev#com: }; the game left by WM_CLOSE"
