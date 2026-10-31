#!/bin/bash
# Suite check for "race_be" (tests/mw/scripts/race_be.txt, TRACKER M4-22): the module under test is named (L-020);
# every wait in time; the Black Edition's three races 21.1.1, 21.2.1 and 21.2.2 (raceinfo's EventID: Attrib's hash
# of the name) each reached GO, the player's racer finished over the line with the game's AI driving (no racewin in
# the run) and the race's end screen came; the game left by WM_CLOSE.  Prints each race's length, car, place and time.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -qE '^\[mwrace\] f=[0-9]+ racewin' "$LOG" && fail "racewin finished a race, not the line"
out=""
for e in 21.1.1:1fdf6afd 21.2.1:40608ee5 21.2.2:068305f0; do
  n=${e%%:*} h=${e##*:}
  for m in GO finished "end screen"; do grep -q "LOG race_be: $n $m\$" "$LOG" || fail "$n: no '$m'"; done
  r=$(awk -v n="$n" '$0 ~ "LOG race_be: " n " GO$" { g = 1; next } g && /raceinfo/ { print; exit }' "$LOG")
  echo "$r" | grep -q "event $h " || fail "$n: the race at its GO is not event $h: $r"
  s=$(awk -v n="$n" '$0 ~ "LOG race_be: " n " finished$" { g = 1; next } g && /racestatus/ { print; exit }' "$LOG")
  echo "$s" | grep -qE 'player 1: #0 100\.0% finished' || fail "$n: the player's racer did not finish: $s"
  len=$(echo "$r" | sed -E 's/.* length ([0-9]+) m .*/\1/'); car=$(echo "$r" | sed -E 's/.* car ([^;]+);.*/\1/')
  t=$(echo "$s" | sed -E 's/.*#0 100\.0% finished ([0-9.]+)s.*/\1/'); w=$(echo "$s" | sed -E 's/.* won ([01]) .*/\1/')
  out="$out $n ($len m, $car): ${t} s, won $w;"
done
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "the Black Edition's three races finished over the line by the game's AI:$out"
