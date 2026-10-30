#!/bin/bash
# Suite check for "bot_segment" (tests/mw/scripts/bot_segment.txt, TRACKER M4-15): one segment of the career bot from a
# save, then a save at the safehouse: the module is named; no wait ran out; the bot stopped by itself (not at game
# over) and then saved; the game left by WM_CLOSE.  Prints why the bot stopped and the gates at the end.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q 'screen GameOver.fng' "$LOG" && fail "game over: $(grep -m1 -E 'bot stops' "$LOG")"
grep -qF -- '-> done: saved' "$LOG" || fail "the bot did not save"
grep -q 'LOG bot_segment: saved' "$LOG" || fail "no 'bot_segment: saved' marker"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
echo "$(grep -m1 -E -- '-> done: [^s]' "$LOG" | sed 's/.*-> done: //'); busted $(grep -c 'screen BUSTED_OVERLAY.fng' "$LOG");" \
  "$(grep -E '\[mwbot\] .* gates \(asked\)' "$LOG" | tail -1 | sed 's/.*gates (asked): //')"
