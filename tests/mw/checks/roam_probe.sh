#!/bin/bash
# Suite check for "roam_probe" (tests/mw/scripts/roam_probe.txt, TRACKER M10-05, M10-06): the module under test is
# named (L-020); the career saved in free roam loaded into free roam; `roam probe` found the player's car (its rigid
# body, matrix and controls) standing and again with LEFT held (the steering control at -1); every `roam spawn` made a
# car of driver class 4 (DRIVER_NONE) that is in the game's vehicle list; the cars were posed every frame until
# `roam unspawn` (each shown 600 frames or more) and the game moved none of them more than 0.25 m between two poses
# (the is300's own settling; the second car's model may come later and settle more: it is only logged); the game
# left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
miss=$(inorder 'LOG roam_probe: free roam' ' probe: ISimable 0x' 'controls: +00=' ' spawn ' 'LOG roam_probe: done') || \
  fail "missing, or out of order: $miss"
grep -qE '^\[roam\]   IInput 0x[0-9a-f]+ \(vtable 0x[0-9a-f]+\) controls: \+00=[-0-9.]+ \+04=-1\.000' "$LOG" || \
  fail "no probe saw the steering control at -1 with LEFT held"
n=$(grep -cE '^\[roam\] f=[0-9]+ spawn [a-z0-9]+ \(key [0-9a-f]{8}\): ISimable 0x[0-9a-f]+, .*driver class 4, active [01], listed 1;' "$LOG")
[ "$n" -ge 5 ] || fail "$n spawns made a listed car of driver class 4 (want 5): $(grep -m1 -E ' spawn .*(made none|listed 0)' "$LOG")"
grep -qE ' spawn .*the game made none' "$LOG" && fail "a spawn made no car: $(grep -m1 'the game made none' "$LOG")"
short=$(awk '/ test[0-9]+.s car \(.*\) away: unspawn, [0-9]+ frames shown/ { for (i = 1; i <= NF; i++) if ($i == "frames") { if ($(i - 1) + 0 < 600) print; break } }' "$LOG" | head -1)
[ -z "$short" ] || fail "a car was not posed 600 frames: $short"
[ "$(grep -cE ' test[0-9]+.s car \(.*\) away: unspawn' "$LOG")" -ge 5 ] || fail "fewer than 5 cars went away at the unspawn"
moved=$(awk '/\[roam\]   test car 0 \(.*\): at .* the game moved it at most [0-9.]+ m/ { for (i = 1; i <= NF; i++) if ($i == "most") v = $(i + 1) }
             END { print v + 0 }' "$LOG")
awk -v m="$moved" 'BEGIN { exit !(m <= 0.25) }' || fail "the game moved the first car $moved m between two poses (want 0.25 m or less)"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "free roam; the player's car probed (steering -1 with LEFT held); $n cars of driver class 4 made, listed and posed until" \
  "the unspawn, the first moved by the game at most $moved m between two poses; the game left by WM_CLOSE"
