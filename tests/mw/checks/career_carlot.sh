#!/bin/bash
# Suite check for "career_carlot" (tests/mw/scripts/career_carlot.txt, TRACKER M4-11), a start seeded with the save
# career_safehouse.txt left, run with NFSU2_FNG_LOG=3: the module under test is named (L-020); every wait in time; the
# profile's load logged its car tables (runtime/mw/cars_mw.c), with no repair (2 under NFSU2_TEST_CARS_BAD=1, each
# named); the safehouse's Car Select opened; the car lot engaged at its gate; its numbers "N / 32" were shown from 1
# to 32 in turn and back to 1; the lot was left for free roam; all in that order; the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
cars=$(grep -m1 '^\[cars\] profile loaded: ' "$LOG") || fail "no [cars] line at the profile's load"
want=0; grep -qE '^\[cars\] f=[0-9]+ TEST: ' "$LOG" && want=2
echo "$cars" | grep -qE "; $want repaired\$" || fail "want $want repaired at the load: $cars"
[ "$(grep -c '^\[cars\] car [0-9]* .* past the ' "$LOG")" -eq "$want" ] || fail "want $want repair lines"
echo "$cars" | grep -q "the career's car 0x[0-9a-f]* is one of them" || fail "the career's car is not among the records: $cars"
miss=$(inorder '[cars] profile loaded: ' 'waitfng Car_Select.fng:ee053562 satisfied' 'LOG career_carlot: free roam' \
  'waitfng Car_Select.fng:d9d6b954 satisfied' 'LOG career_carlot: the car lot' 'LOG career_carlot: every car shown' \
  'LOG career_carlot: left the lot') || fail "missing, or out of order: $miss"
# the lot's car numbers ("N / 32": string 0x18 of Car_Select.fng) from the free roam (the first is drawn as the lot
# opens, before "the car lot") to "every car shown"
seq=$(awk '/LOG career_carlot: free roam$/ { p = 1 } /LOG career_carlot: every car shown$/ { p = 0 }
  p && /Car_Select.fng shows/ { n = 0; for (i = 1; i <= NF; i++) if ($i ~ /^:00000018\x27[0-9]+$/) { n++; v[n] = substr($i, 11) }
    if (n >= 3) printf "%s/%s ", v[3], v[1] }' "$LOG")
first=$(echo "$seq" | awk '{ print $1 }'); last=$(echo "$seq" | awk '{ print $NF }')
[ "$first" = "1/32" ] || fail "the lot did not open on its first of 32 cars: $first"
for i in $(seq 2 32); do echo " $seq" | grep -q " $i/32 " || fail "car $i of 32 never shown: $seq"; done
[ "$last" = "1/32" ] || fail "RIGHT 32 times did not come back to the first car: $last"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "${cars#\[cars\] }; the car lot showed its 32 cars in turn and the first again; the lot left; the game left by WM_CLOSE"
