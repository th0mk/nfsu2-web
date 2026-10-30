#!/bin/bash
# Suite check for "bot_impound" (tests/mw/scripts/bot_impound.txt, TRACKER M4-48): busts with the car standing, from a
# safehouse save: the module is named; no wait ran out; the bot never went round Car_Select.fng more than twice; it
# stopped either at the busts asked for with the current car one that can be driven (an Out of Jail marker spent, or
# another car taken after an impound) or at game over (GameOver.fng: no car left); the memory pools were logged at each
# pursuit's end (M4-35); the game left by WM_CLOSE.  Prints
# the busts, the markers spent, the impounds, the cars taken and how it ended.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q '\[mwbot\] f=[0-9]* bot bust: ' "$LOG" || fail "no 'bot bust' line"
again=$(grep -c 'Car_Select.fng again after Enter' "$LOG")
[ "$again" -le 2 ] || fail "Car_Select.fng came back $again times after an Enter (a loop)"
grep -qF -- '-> done: Car_Select: no car was taken' "$LOG" && fail "no car was taken in Car_Select.fng"
spent=$(grep -c 'an Out of Jail marker spent' "$LOG"); imp=$(grep -c 'Car_Select.fng: the current car .* is impounded' "$LOG")
took=$(grep -oE 'the current car: handle 0x[0-9a-f]+ \(the car picked\)' "$LOG" | wc -l)
busts=$(grep -c 'screen BUSTED_OVERLAY.fng' "$LOG")
pur=$(grep -c '\[mwbot\] .* pursuit over (' "$LOG"); pools=$(grep -c '\[mwpool\] .* frame arena' "$LOG")
[ "$pools" -ge "$pur" ] || fail "the pools were logged $pools times for $pur pursuits' ends"
last=$(grep -E '\[mwbot\] .* bust: [0-9]+ of [0-9]+ busts' "$LOG" | tail -1 | sed 's/.*bust: //')
if grep -qF -- '-> done: game over' "$LOG"; then
  grep -q 'screen GameOver.fng' "$LOG" || fail "game over without GameOver.fng"
  end="game over (GameOver.fng), the bot stopped there"
else
  grep -qF -- '-> done: the busts asked for' "$LOG" || fail "the bot did not stop at the busts asked for: $(grep -m1 -E '\[mwbot\] .* -> done' "$LOG")"
  echo "$last" | grep -q ', drivable;' || fail "the current car at the end is not one that can be driven: $last"
  [ $((spent + imp)) -ge 1 ] || fail "no strike reached the car's most: no marker spent and no impound ($last)"
  [ "$imp" -eq 0 ] || [ "$took" -ge 1 ] || fail "an impound, and no other car taken in Car_Select.fng"
  end="the busts asked for, then: $last"
fi
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
main=$(grep -E '\[mwpool\] .* "Main Pool"' "$LOG" | tail -1 | grep -oE 'peak [0-9]+ KB')
echo "busted $busts; Out of Jail markers spent $spent; impounds $imp; cars taken in Car_Select $took (came back $again);" \
  "pools logged at $pools of $pur pursuits' ends (Main Pool ${main:-?}); $end"
