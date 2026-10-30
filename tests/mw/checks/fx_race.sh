#!/bin/bash
# Suite check for "fx_race" (tests/mw/scripts/fx_race.txt, TRACKER M5-05/M5-06): the module under test is named
# (L-020), the race came to GO and ran its 1,500 frames with no racer finished, and the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, ' "$LOG" || fail "no module line naming its revision"
grep -qF 'fx_race: GO' "$LOG" || fail "the race did not reach GO"
grep -qF 'fx_race: 1500 frames' "$LOG" || fail "the 1,500 frames after GO did not pass"
st=$(grep -E '^\[mwrace\] f=[0-9]+ racestatus' "$LOG" | tail -1)
[ -n "$st" ] || fail "no race status at the end"
echo "$st" | grep -qE 'racestatus flow 6 racers [0-9]+ active 1 ' || fail "the race was not on at the end: $st"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
fx=$(grep -cF '[d3dxfx] the effects are the HLE' "$LOG")
echo "the race ran 1,500 frames after GO and the game left by WM_CLOSE; effects: $([ "$fx" -gt 0 ] && echo HLE || echo oracle)"
