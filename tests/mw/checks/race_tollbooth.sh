#!/bin/bash
# Suite check for "race_tollbooth" (tests/mw/scripts/race_tollbooth.txt, TRACKER M4-06): the module under test is
# named (L-020); every pass launched the first Challenge Series event, a Tollbooth Time Trial (read off the list's
# last drawn strings before the launch: the kind's label 0x5444F256 and the counter "1 / 69"), loaded, reached GO,
# was ended by the shortcut finish (racewin: no tollbooth checkpoint or time limit is tested here) and showed the
# race's end screen, every wait in time; no stall report; the game left by WM_CLOSE.  Prints the passes, the end
# screens and the load times (the transmission's Enter to the race, GameFlow 6, and to GO, in seconds of the page's
# clock).
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q 'LOG race_tollbooth: done$' "$LOG" || fail "the runs did not end"
n=$(grep -c 'LOG race_tollbooth: event$' "$LOG")
[ "$n" -gt 0 ] || fail "no pass picked the event"
for m in load racing GO results; do
  k=$(grep -c "LOG race_tollbooth: $m\$" "$LOG")
  [ "$k" = "$n" ] || fail "$k of $n passes reached '$m'"
done
k=$(grep -cE '^\[mwrace\] f=[0-9]+ racewin: racer 1 finished .*all humans done 1: EnterPostRaceFlow' "$LOG")
[ "$k" = "$n" ] || fail "$k of $n passes had racewin finish racer 1 through EnterPostRaceFlow"
grep -qF '[watchdog] no Present' "$LOG" && fail "a stall: $(grep -m1 -F '[watchdog] no Present' "$LOG")"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
# the event each pass launched: the list's last drawn strings before its "event" log line, the kind's label and the
# "N / M" counter's numbers (label 0x18, drawn twice each)
ev=$(awk '/^\[fng\] f=[0-9]+ ChallengeSeries.fng shows / { s = $0 }
          /LOG race_tollbooth: event$/ { k = (s ~ /:5444f256'"'"'/) ? "tollbooth" : "other"; c = "";
            r = s; while (match(r, /:00000018'"'"'[0-9]+/)) { c = c " " substr(r, RSTART + 10, RLENGTH - 10); r = substr(r, RSTART + RLENGTH) }
            printf "%s%s\n", k, c }' "$LOG")
k=$(echo "$ev" | grep -xE 'tollbooth( 69| 1)+' | grep -E ' 1( |$)' | grep -cE ' 69( |$)')
[ "$k" = "$n" ] || fail "$k of $n passes launched event 1 of 69 drawn as a Tollbooth Time Trial (label 5444f256): $(echo "$ev" | sort | uniq -c | tr '\n' ';')"
t="the Challenge Series' event 1 of 69, a Tollbooth Time Trial (label 5444f256 drawn)"
# the end screen each pass came to: the last screen line before its "results" log line
ends=$(awk '/^\[fng\] f=[0-9]+ screen / { s = $4 } /LOG race_tollbooth: results$/ { printf "%s ", s }' "$LOG")
w=$(echo $ends | tr ' ' '\n' | grep -c '^InGamePhotoMaster.fng$')
awk -v n="$n" -v t="$t" -v w="$w" '/LOG race_tollbooth: load$/ { match($0, /t=[0-9]+ms/); l = substr($0, RSTART + 2, RLENGTH - 4) }
     /LOG race_tollbooth: racing$/ { match($0, /t=[0-9]+ms/); r = substr($0, RSTART + 2, RLENGTH - 4); x = (r - l) / 1000; s += x; c++;
                                 if (c == 1 || x < lo) lo = x; if (x > hi) hi = x }
     /LOG race_tollbooth: GO$/ { match($0, /t=[0-9]+ms/); g = (substr($0, RSTART + 2, RLENGTH - 4) - l) / 1000; sg += g }
     END { printf "%d races of %s loaded, reached GO, were ended by racewin (a shortcut finish) and showed the end screen (%d the winner'"'"'s); load to the race %.1f s mean (%.1f..%.1f), to GO %.1f s mean; ", n, t, w, s / c, lo, hi, sg / c }' "$LOG"
echo "the game left by WM_CLOSE"
