#!/bin/bash
# Suite check for "race_drag" (tests/mw/scripts/race_drag.txt, TRACKER M4-05): the module under test is named
# (L-020); every pass picked its track, loaded, reached GO, had the autopilot on ("autopilot on" after its GO) and
# moved (a "where" speed of 30 km/h or more), and came to the race's end screen with no racewin (the race's own script
# ended it), every wait in time; no stall report; the game left by WM_CLOSE.  The race's end is any end screen: the
# passes the player did not finish are printed, a Totaled one (PostRace_Results.fng drawing label 0xB7B75185,
# "Totaled") by name.  Also prints where the player came (racestatus at the end screen), the top speed logged on the
# way and the load times.
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
# each pass, from its track line on: the autopilot on after GO, its top "where" speed, the end screen and whether it
# drew "Totaled", the player's share and finish at the end (racestatus)
per=$(awk '/LOG race_drag: track / { p++; ai[p] = 0; go[p] = 0; v[p] = 0; tot[p] = 0; st[p] = "-" }
           /LOG race_drag: GO$/ { go[p] = 1 }
           /^\[mwrace\] f=[0-9]+ autopilot on$/ { if (go[p]) ai[p] = 1 }
           /^\[mwrace\] f=[0-9]+ where .* speed / { x = $(NF-1) + 0; if (x > v[p]) v[p] = x }
           /^\[fng\] f=[0-9]+ PostRace_Results.fng shows / { last = $0 }
           /^\[fng\] f=[0-9]+ screen / { scr = $4 }
           /LOG race_drag: results$/ { tot[p] = (scr == "PostRace_Results.fng" && last ~ /:b7b75185'"'"'/); done[p] = 1 }
           /^\[mwrace\] f=[0-9]+ racestatus .* player 1: #0 / { if (done[p] && st[p] == "-") { match($0, /player 1: #0 [0-9.]+%( finished)?/);
                                                                 st[p] = substr($0, RSTART + 13, RLENGTH - 13); gsub(/ /, "_", st[p]) } }
           END { for (i = 1; i <= p; i++) printf "%d %d %.1f %d %s\n", i, ai[i], v[i], tot[i], st[i] }' "$LOG")
k=$(echo "$per" | awk '$2 == 1' | wc -l)
[ "$k" = "$n" ] || fail "$k of $n passes had the autopilot on after GO"
k=$(echo "$per" | awk '$3 >= 30' | wc -l)
[ "$k" = "$n" ] || fail "$k of $n passes logged a speed of 30 km/h or more: $(echo "$per" | awk '$3 < 30 { printf "pass %d %s km/h; ", $1, $3 }')"
out=$(echo "$per" | awk '$5 !~ /finished$/ { printf "%spass %d at %s%s", (o++ ? ", " : ""), $1, $5, ($4 ? " Totaled" : " not finished") }')
t=$(grep -m1 -oE 'LOG race_drag: track [0-9a-f]+' "$LOG" | awk '{print $4}')
fin=$(grep -cE '^\[mwrace\] f=[0-9]+ racestatus .* player 1: #0 100\.0% finished' "$LOG")
won=$(awk '/^\[fng\] f=[0-9]+ screen / { s = $4 } /LOG race_drag: results$/ { if (s == "InGamePhotoMaster.fng") w++ } END { print w + 0 }' "$LOG")
v=$(grep -oE '^\[mwrace\] f=[0-9]+ where .* speed [0-9.]+ km/h' "$LOG" | awk '{print $(NF-1)}' | sort -n | tail -1)
awk -v n="$n" -v t="$t" -v v="$v" -v f="$fin" -v w="$won" '/LOG race_drag: load$/ { match($0, /t=[0-9]+ms/); l = substr($0, RSTART + 2, RLENGTH - 4) }
     /LOG race_drag: racing$/ { match($0, /t=[0-9]+ms/); r = substr($0, RSTART + 2, RLENGTH - 4); x = (r - l) / 1000; s += x; c++;
                                 if (c == 1 || x < lo) lo = x; if (x > hi) hi = x }
     /LOG race_drag: GO$/ { match($0, /t=[0-9]+ms/); g = (substr($0, RSTART + 2, RLENGTH - 4) - l) / 1000; sg += g }
     END { printf "%d drags on track %s driven to the end screen: %d finished by the AI, %d won (top speed logged %s km/h); load to the race %.1f s mean (%.1f..%.1f), to GO %.1f s mean; ", n, t, f, w, v, s / c, lo, hi, sg / c }' "$LOG"
[ -n "$out" ] && printf 'not finished: %s; ' "$out"
echo "the game left by WM_CLOSE"
