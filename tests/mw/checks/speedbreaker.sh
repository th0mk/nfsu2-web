#!/bin/bash
# Suite check for "speedbreaker" (tests/mw/scripts/speedbreaker.txt, TRACKER M4-19): the module under test is named
# (L-020); every wait in time; the race reached GO; Right Ctrl switched the Speedbreaker on and off, G on; the
# charge then fell while on, a tenth in no less than 45 frames (0.1 a second: 60 at 60 frames a second; the drain
# runs on the game's other clock, so a slow frame shortens a step) and 55 to 65 on average, until the game switched
# it off; it then rose only while the car was above 100 mph, a tenth in no less than 140 frames (1/25 a second:
# 150); the game left by WM_CLOSE.  Prints the drain and refill rates.
LOG="$1"
fail(){ echo "$1"; exit 1; }
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
ON='LOG speedbreaker: on by G'; OFF='LOG speedbreaker: switched off by the game'; UP='LOG speedbreaker: refilled'
RISEN='breaker wait rise: satisfied'     # (the refill's lines end there: the wait logs the state once more after it)
miss=$(inorder 'LOG speedbreaker: GO' 'breaker wait on: satisfied' 'LOG speedbreaker: on by Right Ctrl' \
  'breaker wait off: satisfied' 'LOG speedbreaker: off by Right Ctrl' 'breaker wait on: satisfied' "$ON" \
  'breaker wait off: satisfied' "$OFF" 'breaker wait rise: satisfied' "$UP") || fail "missing, or out of order: $miss"
# the watch lines (one a tenth of the charge, at the frame it is crossed, with the car's speed; a wait's own line in
# the same frame is passed over; the first step, from wherever the charge was, is not a whole tenth)
steps(){ awk -v from="$1" -v to="$2" -v kind="$3" -v min="$4" '
  index($0, from) { on = 1; last = -1; n = 0; next } index($0, to) { on = 0 }
  on && $1 == "[mwbreaker]" && $3 == kind && $4 == "charge" { split($2, a, "="); f = a[2] + 0
    if (f == last) next
    if (++n >= 3 && f - last < min) { print "a tenth in " f - last " frames: " $0; exit 1 } last = f }' "$LOG"; }
rate(){ awk -v from="$1" -v to="$2" -v kind="$3" '
  index($0, from) { on = 1; n = 0; next } index($0, to) { on = 0 }
  on && $1 == "[mwbreaker]" && $3 == kind && $4 == "charge" { split($2, a, "="); f = a[2] + 0; if (f == last) next
    if (++n >= 3) { s += f - last; k++ } last = f }
  END { if (k) printf "%.0f frames a tenth over %d tenths", s / k, k; else printf "no whole tenth" }' "$LOG"; }
bad=$(steps "$ON" "$OFF" on 45) || fail "drain too fast: $bad"
mean=$(rate "$ON" "$OFF" on | awk '{ print $1 }')
[ "$mean" -ge 55 ] 2>/dev/null && [ "$mean" -le 65 ] || fail "drain: $(rate "$ON" "$OFF" on), not 0.1 a second"
bad=$(steps "$OFF" "$RISEN" off 140) || fail "refill too fast: $bad"
slow=$(awk -v from="$OFF" -v to="$UP" 'index($0, from) { e = 1; next } index($0, to) { e = 0 }
  e && $1 == "[mwbreaker]" && $3 == "off" && $4 == "charge" { c = $5 + 0; if (c > last + 0.05 && $7 + 0 <= 100.0) { print; exit } last = c }' "$LOG")
[ -z "$slow" ] || fail "the charge rose at 100 mph or less: $slow"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
echo "drain: $(rate "$ON" "$OFF" on); refill: $(rate "$OFF" "$RISEN" off)"
