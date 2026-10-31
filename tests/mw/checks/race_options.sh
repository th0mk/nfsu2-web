#!/bin/bash
# Suite check for "race_options" (tests/mw/scripts/race_options.txt, TRACKER M4-07): the module under test is named
# (L-020); every wait in time; five Custom Races, each set up on the screens, reached GO and the winner's screen; at
# each GO the race's parameters as the game reads them (raceinfo, runtime/mw/career_mw.c) are the ones its screens
# set: a circuit's laps, the traffic level (None 0, Minimum 10, Moderate 30, Maximum 50, the values of the race's
# TrafficLevel field), the difficulty (the game's enum: Easy 0, Medium 1, Hard 2), catch-up (On 1, Off 0), and its
# racers are the opponents and the player; over 3000 frames of the AI driving there is never a traffic car (driver
# class 1) with None, and at least one comes with Moderate and with Maximum (Minimum's count is printed: one in 1500
# frames is not sure to come); the races used the three cars the car list offers with no profile; the game left by
# WM_CLOSE.  Prints each race's line.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
for m in GO results; do
  n=$(grep -cE "LOG race_options: config [A-E] $m\$" "$LOG")
  [ "$n" = 5 ] || fail "$n of 5 races reached '$m'"
done
out=$(awk '
  function v(k,   i) { for (i = 1; i < NF; i++) if ($i == k) return $(i + 1); return "" }
  BEGIN { T["none"] = 0; T["minimum"] = 10; T["moderate"] = 30; T["maximum"] = 50; D["easy"] = 0; D["medium"] = 1; D["hard"] = 2
          C["on"] = 1; C["off"] = 0 }
  /LOG race_options: config [A-E] [a-z]+ traffic/ {
    for (i = 1; i <= NF; i++) if ($i == "config") break
    cfg = $(i + 1); kind = $(i + 2); tr = $(i + 4); op = $(i + 6); df = $(i + 8); cu = $(i + 10); lp = $(i + 12); k = 0; next }
  /^\[mwrace\] f=[0-9]+ raceinfo / && cfg != "" {
    k++
    if (k == 1) {
      bad = ""
      if (kind == "circuit" && v("laps") != lp) bad = bad " laps " v("laps") " (want " lp ")"
      if (v("traffic") != T[tr]) bad = bad " traffic " v("traffic") " (want " T[tr] ")"
      if (v("difficulty") != D[df]) bad = bad " difficulty " v("difficulty") " (want " D[df] ")"
      if (v("catchup") != C[cu]) bad = bad " catchup " v("catchup") " (want " C[cu] ")"
      if (v("racers") + 0 != op + 1) bad = bad " racers " v("racers") " (want " op + 1 ")"
      car = v("car"); sub(/;$/, "", car)
      if (!(car in seen)) ncars++
      seen[car] = 1
      line = cfg ": " kind " laps " v("laps") " traffic " v("traffic") " difficulty " v("difficulty") " catchup " v("catchup") " racers " v("racers") " car " car
      t = 0
    } else {
      match($0, / 1:[0-9]+ /); c = substr($0, RSTART + 3, RLENGTH - 4) + 0; if (c > t) t = c
      if (k == 3) {
        if (tr == "none" && t != 0) bad = bad " " t " traffic cars with None"
        if ((tr == "moderate" || tr == "maximum") && t == 0) bad = bad " no traffic car with " tr
        print line ", traffic cars at most " t (bad != "" ? "; WRONG:" bad : "")
        if (bad != "") nbad++
        n++; cfg = ""
      }
    } }
  END { print "races " n + 0 " wrong " nbad + 0 " cars " ncars + 0 }' "$LOG")
echo "$out" | grep -q '^races 5 wrong 0 cars 3$' || fail "the races did not take their options: $(echo "$out" | tr '\n' '|')"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "five Custom Races took every Quick Race option value from their screens: $(echo "$out" | grep -v '^races' | tr '\n' '|')"
