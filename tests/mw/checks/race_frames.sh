#!/bin/bash
# Suite check for "race_frames" (tests/mw/scripts/race_frames.txt, TRACKER M4-01): the module under test is named
# (L-020), the race reached GO, and over the 10,000 frames after it, all in the race (GameFlow 6), the game's main
# loop passed its per-frame anchor once a Present, give or take one (the [mwrace] frames lines of
# runtime/mw/career_mw.c: Presents from com.c, passes from runtime/debug.c guest_hook); the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q 'LOG race_frames: GO' "$LOG" || fail "the race never reached GO"
grep -q '\[mwrace\] f=[0-9]* autopilot on' "$LOG" || fail "the AI never took the player's car"
fr=$(grep -E '^\[mwrace\] f=[0-9]+ frames: ' "$LOG")
[ "$(echo "$fr" | grep -c .)" = 2 ] || fail "not two [mwrace] frames lines"
echo "$fr" | grep -qv 'flow 6$' && fail "a frames line outside the race: $(echo "$fr" | grep -v 'flow 6$' | head -1)"
read -r p0 m0 p1 m1 <<< "$(echo "$fr" | awk '{ for (i = 1; i <= NF; i++) { if ($i == "presents") p[NR] = $(i+1); if ($i == "passes") m[NR] = $(i+1) } }
  END { print p[1], m[1], p[2], m[2] }')"
dp=$((p1 - p0)); dm=$((m1 - m0)); d=$((dm - dp)); [ $d -lt 0 ] && d=$((-d))
[ $dp -ge 10000 ] || fail "only $dp Presents between the frames lines"
[ $d -le 1 ] || fail "$dm anchor passes for $dp Presents"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "in the race: $dm anchor passes for $dp Presents (from Present $p0), the AI driving; the game left by WM_CLOSE"
