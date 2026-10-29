#!/bin/bash
# Suite check for "kb_menu" (tests/mw/scripts/kb_menu.txt, TRACKER M3-07): the module under test is named (L-020),
# every screen of the walk came up in order by keys alone (main menu, its Options sub menu, the options screen, the
# sub menu again, the main menu again), a key held across a focus loss was let go and the game's next keyboard
# snapshot had it up (the [dikbd] snapshot lines of NFSU2_KEY_LOG=1), and the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'waitfng .* TIMEOUT\|tapuntil .* TIMEOUT' "$LOG" && fail "a screen did not come up: $(grep -m1 'TIMEOUT' "$LOG")"
# each screen once, however many of its states the script waited for (MainMenu.fng@HASH: the package's name)
got=$(grep -oE '^\[autotest\] f=[0-9]+ waitfng [A-Za-z_]+\.fng[^ ]* satisfied' "$LOG" | awk '{ sub(/[:@^'\''].*/, "", $4); print $4 }' | uniq | tr '\n' ' ')
want='MainMenu.fng MainMenu_Sub.fng Options.fng MainMenu_Sub.fng MainMenu.fng '
[ "$got" = "$want" ] || fail "the screens came up as '$got', not '$want'"
# the held Down arrow (DIK 0xD0): down in a snapshot before the focus loss, up in a snapshot between the focus loss
# and the focus gain (the script lets go of it only after the gain), and the page's release named it.  The input
# thread prints its release line after letting go, so the game's snapshot line can come first.
awk '
  /\[dikbd\] snapshot .*: key 0xd0 down/ && !out { down = 1 }
  /\[autotest\] f=[0-9]+ nsfocus out/ { out = 1 }
  /\[dikbd\] snapshot .*: key 0xd0 up/ && out && !gain { up = 1 }
  /\[autotest\] f=[0-9]+ nsfocus in/ && out { gain = 1 }
  END { exit !(down && up) }' "$LOG" \
  || fail "the Down arrow held across the focus loss was not down before it and up in the game's snapshot after it"
grep -qF '[key] autotest nsfocus inactive: 1 held key(s) released' "$LOG" || fail "the focus loss released no held key"
grep -qF 'app: quit requested (autotest close) -> WM_CLOSE to the game' "$LOG" || fail "no WM_CLOSE sent"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "main menu -> Options sub menu -> options -> back -> main menu by keys; the key held across the focus loss went up in the game's snapshot; the game left by WM_CLOSE"
