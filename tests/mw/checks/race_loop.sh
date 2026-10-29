#!/bin/bash
# Suite check for "race_loop" (tests/mw/scripts/race_loop.txt, TRACKER M4-04): the module under test is named
# (L-020); 25 quick races each loaded, reached GO, were won through the game's finish path (racewin) and showed the
# winner's post-race screen, every wait in time; no stall report, so the main thread never sat in a function
# (Q-31: 0x684710, the map lookup another recompilation saw spin); the game left by WM_CLOSE.  Prints the load times: from the
# brief's Enter to the race (GameFlow 6) and to GO, in seconds of the page's clock.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
for m in load racing GO results; do
  n=$(grep -c "LOG race_loop: $m\$" "$LOG")
  [ "$n" = 25 ] || fail "$n of 25 races reached '$m'"
done
n=$(grep -cE '^\[mwrace\] f=[0-9]+ racewin: racer [0-9]+ finished .*all humans done 1: EnterPostRaceFlow' "$LOG")
[ "$n" = 25 ] || fail "$n of 25 races were finished by racewin through EnterPostRaceFlow"
# the race's state racewin logs after its finish
n=$(awk '/^\[mwrace\] f=[0-9]+ racewin: / { w = 1; next }
  w && /^\[mwrace\] f=[0-9]+ racestatus / { if ($0 ~ /racestatus flow 6 racers [0-9]+ active 1 won 1: #0 100\.0% finished/) n++; w = 0 }
  END { print n + 0 }' "$LOG")
[ "$n" = 25 ] || fail "$n of 25 races ended with the player first and finished"
# 0x684710..0x684786 is the lookup (a red-black tree's lower bound, map::operator[]); a stall report names each
# thread's eip (runtime/watchdog.c)
grep -qE '^ +thread [0-9]+ .*eip=0x0068471[0-9a-f]|^ +thread [0-9]+ .*eip=0x006847[2-8][0-9a-f]' "$LOG" \
  && fail "a stall report shows a thread in 0x684710 (Q-31): $(grep -m1 -E 'eip=0x006847' "$LOG")"
grep -qF '[watchdog] no Present' "$LOG" && fail "a stall: $(grep -m1 -F '[watchdog] no Present' "$LOG")"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
awk '/LOG race_loop: load$/ { match($0, /t=[0-9]+ms/); l = substr($0, RSTART + 2, RLENGTH - 4) }
     /LOG race_loop: racing$/ { match($0, /t=[0-9]+ms/); r = substr($0, RSTART + 2, RLENGTH - 4); d = (r - l) / 1000; s += d; n++;
                                 if (n == 1 || d < lo) lo = d; if (d > hi) hi = d }
     /LOG race_loop: GO$/ { match($0, /t=[0-9]+ms/); g = (substr($0, RSTART + 2, RLENGTH - 4) - l) / 1000; sg += g }
     END { printf "25 quick races loaded, reached GO, won and showed the post-race screen; load to the race %.1f s mean (%.1f..%.1f), to GO %.1f s mean; ", s / n, lo, hi, sg / n }' "$LOG"
echo "the game left by WM_CLOSE"
