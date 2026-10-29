#!/bin/bash
# Suite check for "pad_menu" (tests/mw/scripts/pad_menu.txt, TRACKER M3-09), and for "pad_menu_boot" with "boot":
# the module under test is named (L-020); the game got exactly one game pad: plugged in after its enumeration found
# none, offered to its input manager's callback and its input set-up run again (pad_menu), or there at the start and
# offered by the enumeration itself (pad_menu_boot); every screen of the walk came up in order by the pad alone
# (main menu, its Options sub menu, the options screen, the sub menu again, the main menu again); and the game left
# by WM_CLOSE.
LOG="$1"; MODE="${2:-late}"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'waitfng .* TIMEOUT\|tappaduntil .* TIMEOUT' "$LOG" && fail "a screen did not come up: $(grep -m1 'TIMEOUT' "$LOG")"
[ "$(grep -c 'CreateDevice GUID .* -> GAMEPAD' "$LOG")" -ge 1 ] || fail "the game made no game pad device"
# the manager's device, then (only at the start) the force-feedback subsystem's: one of each at most
[ "$(grep -c 'CreateDevice GUID .* -> GAMEPAD' "$LOG")" -le 2 ] || fail "the game made more than two game pad devices"
if [ "$MODE" = boot ]; then
  grep -qF '[pad] EnumDevices(type 0x0) -> ' "$LOG" || fail "the game's enumeration at the start offered no pad"
  grep -qF 'connected after the game' "$LOG" && fail "a pad there at the start was offered again later"
else
  grep -qF '[pad] EnumDevices(' "$LOG" && fail "the enumeration offered a pad before the script plugged one in"
  grep -qE "\[pad\] .* connected after the game's enumeration: .* offered to its callback 0x007f1a6b" "$LOG" \
    || fail "the late pad was not offered to the input manager's callback"
  grep -qE "\[pad\] the game's input set-up 0x0063f850 run again on 0x" "$LOG" \
    || fail "the game's input set-up did not run again for the late pad"
fi
got=$(grep -oE '^\[autotest\] f=[0-9]+ (waitfng|tappaduntil) [A-Za-z_]+\.fng[^ ]* satisfied' "$LOG" | awk '{ sub(/[:@^'\''].*/, "", $4); print $4 }' | uniq | tr '\n' ' ')
want='MainMenu.fng MainMenu_Sub.fng Options.fng MainMenu_Sub.fng MainMenu.fng '
[ "$got" = "$want" ] || fail "the screens came up as '$got', not '$want'"
grep -qF 'app: quit requested (autotest close) -> WM_CLOSE to the game' "$LOG" || fail "no WM_CLOSE sent"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "main menu -> Options sub menu -> options -> back -> main menu by the pad ($MODE); the game left by WM_CLOSE"
