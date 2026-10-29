#!/bin/bash
# Suite check for "menu_hold" (tests/mw/scripts/menu_hold.txt, TRACKER M7-29): the module under test is named
# (L-020), the main menu came up, its camera's swing-in (the [trace] lines from the menu's first frame: the last one
# whose position differs from the one before) was over 5.8 to 6.3 s after the menu came up both in wall time (the
# lines' t, ms) and in RealTimer (rt, 1/4000 s): the front end's cameras on the wall clock at any frame rate;
# RealTimeFrames went 60 per second of RealTimer over the hold, give or take 2 (the peeks at both ends); the game
# left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q 'waitfng MainMenu.fng satisfied' "$LOG" || fail "the main menu never came up"
read -r wall game moves lines <<< "$(grep -E '^\[trace\] f=' "$LOG" | awk '
  { for (i = 2; i <= NF; i++){ split($i, kv, "="); v[kv[1]] = kv[2] }
    if (NR == 1){ t0 = v["t"]; r0 = v["rt"] }
    k = v["x"] " " v["y"] " " v["z"]
    if (k != last){ last = k; tw = v["t"]; tr = v["rt"]; n++ } }
  END { if (NR) printf "%.3f %.3f %d %d\n", (tw - t0) / 1000, (tr - r0) / 4000, n, NR }')"
[ -n "$lines" ] || fail "no [trace] lines of the menu's camera"
[ "$moves" -ge 20 ] || fail "the camera moved on $moves of $lines trace lines: no swing-in seen"
awk -v w="$wall" -v g="$game" 'BEGIN { exit !(w >= 5.8 && w <= 6.3 && g >= 5.8 && g <= 6.3) }' ||
  fail "the camera's swing-in ended $wall s (wall) and $game s (RealTimer) after the menu came up, not 6"
read -r drtf drt <<< "$(grep -E 'peek \[0x(008f82f4|00925b14)\]' "$LOG" | awk '
  { v = $0; sub(/.*\(/, "", v); sub(/,.*/, "", v) }
  /0x008f82f4/ { f[++nf] = v } /0x00925b14/ { t[++nt] = v }
  END { if (nf >= 2 && nt >= 2) print f[nf] - f[1], t[nt] - t[1] }')"
[ -n "$drt" ] || fail "not two peeks of RealTimeFrames and RealTimer"
awk -v a="$drtf" -v b="$drt" 'BEGIN { d = a - b * 60 / 4000; exit !(b > 0 && d >= -2 && d <= 2) }' ||
  fail "RealTimeFrames went $drtf for $drt RealTimer units (60 a second wants $((drt * 60 / 4000)))"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
fps=$(grep -E '^\[status\] menu_hold60 ' "$LOG" | sed -E 's/.* fps=([0-9.]+).*/\1/')
echo "the camera's swing-in over ${wall} s (wall) and ${game} s (RealTimer) after the menu came up; RealTimeFrames +$drtf for $drt RealTimer units over the hold at ${fps:-?} fps; the game left by WM_CLOSE"
