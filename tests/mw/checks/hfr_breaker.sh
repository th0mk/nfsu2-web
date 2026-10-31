#!/bin/bash
# Suite check for "hfr_breaker" (tests/mw/scripts/hfr_breaker.txt, TRACKER M7-32): the module under test is named
# (L-020); every wait in time; GO, the Speedbreaker on and off by its keys, switched off by the game at an empty
# charge and refilled by 0.4; no stall report.  The rates compare from the [hfrtr] lines (tools/mw/hfr_ab.py).
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
for m in GO 'on by Right Ctrl' 'off by Right Ctrl' 'on by G' 'switched off by the game' refilled; do
  grep -q "LOG hfr_breaker: $m\$" "$LOG" || fail "no '$m'"
done
grep -qF '[watchdog] no Present' "$LOG" && fail "a stall: $(grep -m1 -F '[watchdog] no Present' "$LOG")"
mode=$(grep -m1 -F '=== frame rate (Most Wanted)' "$LOG" | sed 's/^.*: mode/mode/; s/ ===$//')
echo "PASS hfr_breaker: ${mode:-no frame-rate report}"
