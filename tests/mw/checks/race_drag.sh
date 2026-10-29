#!/bin/bash
# Suite check for "race_drag" (tests/mw/scripts/race_drag.txt, TRACKER M4-05): the module under test is named
# (L-020); every pass picked its track, loaded, reached GO, was driven by the game's AI to the race's end screen (no
# racewin: the race's own script ends it), every wait in time; no stall report; the game left by WM_CLOSE.  Prints
# the passes, where the player came (racestatus at the end screen: finished or out, won), the top speed logged on
# the way and the load times.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q 'LOG race_drag: done$' "$LOG" || fail "the runs did not end"
grep -q 'racewin' "$LOG" && fail "a shortcut finish: the drags are driven to their end"
n=$(grep -c 'LOG race_drag: track ' "$LOG")
[ "$n" -gt 0 ] || fail "no pass picked a track"
for m in load racing GO results; do
  k=$(grep -c "LOG race_drag: $m\$" "$LOG")
  [ "$k" = "$n" ] || fail "$k of $n passes reached '$m'"
done
k=$(grep -cE '^\[mwrace\] f=[0-9]+ racestatus flow 6 racers [0-9]+ active [01] won [01] player 1: #0 ' "$LOG")
[ "$k" = "$n" ] || fail "$k of $n passes logged the race with the player as racer 1 at the end"
grep -qF '[watchdog] no Present' "$LOG" && fail "a stall: $(grep -m1 -F '[watchdog] no Present' "$LOG")"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
t=$(grep -m1 -oE 'LOG race_drag: track [0-9a-f]+' "$LOG" | awk '{print $4}')
fin=$(grep -cE '^\[mwrace\] f=[0-9]+ racestatus .* player 1: #0 100\.0% finished' "$LOG")
won=$(awk '/^\[fng\] f=[0-9]+ screen / { s = $4 } /LOG race_drag: results$/ { if (s == "InGamePhotoMaster.fng") w++ } END { print w + 0 }' "$LOG")
v=$(grep -oE '^\[mwrace\] f=[0-9]+ where .* speed [0-9.]+ km/h' "$LOG" | awk '{print $(NF-1)}' | sort -n | tail -1)
awk -v n="$n" -v t="$t" -v v="$v" -v f="$fin" -v w="$won" '/LOG race_drag: load$/ { match($0, /t=[0-9]+ms/); l = substr($0, RSTART + 2, RLENGTH - 4) }
     /LOG race_drag: racing$/ { match($0, /t=[0-9]+ms/); r = substr($0, RSTART + 2, RLENGTH - 4); x = (r - l) / 1000; s += x; c++;
                                 if (c == 1 || x < lo) lo = x; if (x > hi) hi = x }
     /LOG race_drag: GO$/ { match($0, /t=[0-9]+ms/); g = (substr($0, RSTART + 2, RLENGTH - 4) - l) / 1000; sg += g }
     END { printf "%d drags on track %s driven to the end screen: %d finished by the AI, %d won (top speed logged %s km/h); load to the race %.1f s mean (%.1f..%.1f), to GO %.1f s mean; ", n, t, f, w, v, s / c, lo, hi, sg / c }' "$LOG"
echo "the game left by WM_CLOSE"
