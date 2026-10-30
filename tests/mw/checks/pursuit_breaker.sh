#!/bin/bash
# Suite check for "pursuit_breaker" (tests/mw/scripts/pursuit_breaker.txt, TRACKER M4-17): the module under test is
# named (L-020); the pursuit came, the car was put before the breaker and the game posted MBreakerStopCops in time (the
# wait's answer); the pursuit ended (busted or evaded, the game's sites); the game left by WM_CLOSE.  Prints the
# breaker's line, how the pursuit ended and the rap sheet before and after.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'LOG pursuit_breaker: at the breaker' "$LOG" || fail "the car was not put before the breaker"
grep -q 'pursuit wait breaker: TIMEOUT' "$LOG" && fail "no breaker came down in 900 frames: $(grep -m1 -E 'teleport to' "$LOG")"
grep -q 'pursuit wait breaker: satisfied' "$LOG" || fail "no answer from the breaker wait"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q 'LOG pursuit_breaker: over' "$LOG" || fail "no 'pursuit_breaker: over' marker"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
end=$(grep -oE '^\[mwpursuit\] f=[0-9]+ (evaded|busted): ' "$LOG" | tail -1 | awk '{ print $3 }' | tr -d :)
rap(){ grep -E '^\[mwrap\] f=[0-9]+ rapsheet \(asked\)' "$LOG" | sed -n "$1p" | sed 's/.*(asked)://'; }
echo "$(grep -m1 -E 'breaker: MBreakerStopCops' "$LOG" | sed 's/.*breaker: //'); the pursuit ${end:-ended}; rap sheet" \
  "before:$(rap 1); after:$(rap 2)"
