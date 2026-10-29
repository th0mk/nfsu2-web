#!/bin/bash
# Suite check for "race_frames" (tests/mw/scripts/race_frames.txt, TRACKER M4-01): the module under test is named
# (L-020), the race reached GO, and over the 10,000 frames after it the game's main loop passed its per-frame anchor
# once a Present, give or take one (the [mwrace] frames lines of runtime/mw/career_mw.c: Presents from com.c,
# passes from runtime/debug.c guest_hook); all of it in the race: both frames lines in GameFlow 6 (which the
# post-race screens keep too), no post-race screen in between (PostRace_Results.fng after a loss,
# InGamePhotoMaster.fng after a win), and the race status after the window shows the race on, no racer finished,
# the player racer 1 (Game.GetRacerIndex); the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q 'LOG race_frames: GO' "$LOG" || fail "the race never reached GO"
grep -q '\[mwrace\] f=[0-9]* autopilot on' "$LOG" || fail "the AI never took the player's car"
fr=$(grep -E '^\[mwrace\] f=[0-9]+ frames: ' "$LOG")
[ "$(echo "$fr" | grep -c .)" = 2 ] || fail "not two [mwrace] frames lines"
echo "$fr" | grep -qv 'flow 6$' && fail "a frames line outside the race: $(echo "$fr" | grep -v 'flow 6$' | head -1)"
read -r f0 p0 m0 f1 p1 m1 <<< "$(echo "$fr" | awk '{ f[NR] = substr($2, 3); for (i = 1; i <= NF; i++) { if ($i == "presents") p[NR] = $(i+1); if ($i == "passes") m[NR] = $(i+1) } }
  END { print f[1], p[1], m[1], f[2], p[2], m[2] }')"
[ -n "$m1" ] || fail "the frames lines lack their counts: $(echo "$fr" | head -1)"
dp=$((p1 - p0)); dm=$((m1 - m0)); d=$((dm - dp)); [ $d -lt 0 ] && d=$((-d))
[ $d -le 1 ] || fail "$dm anchor passes for $dp Presents"
# the race went on over the whole window
pr=$(awk -v a="$f0" -v b="$f1" '/^\[fng\] f=[0-9]+ screen (PostRace|InGamePhotoMaster)/ { f = substr($2, 3) + 0; if (f >= a + 0 && f <= b + 0) { print; exit } }' "$LOG")
[ -z "$pr" ] || fail "a post-race screen inside the window (frames $f0..$f1): $pr"
st=$(awk -v b="$f1" '/^\[mwrace\] f=[0-9]+ racestatus/ { if (substr($2, 3) + 0 >= b + 0) { print; exit } }' "$LOG")
[ -n "$st" ] || fail "no race status after the window"
echo "$st" | grep -qE 'racestatus flow 6 racers [0-9]+ active 1 won 0 player 1: #0 ' || fail "the race was not on at the window's end: $st"
echo "$st" | grep -q ' finished' && fail "a racer finished inside the window: $st"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
at=$(echo "$st" | sed -E 's/.*: #0 ([0-9.]+%).*/\1/')
echo "in the race (the player at $at of it after the window, no racer finished): $dm anchor passes for $dp Presents (from Present $p0), the AI driving; the game left by WM_CLOSE"
