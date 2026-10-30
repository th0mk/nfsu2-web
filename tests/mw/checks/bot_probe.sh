#!/bin/bash
# Suite check for "bot_probe" (tests/mw/scripts/bot_probe.txt, TRACKER M4-15): one step of the career bot (BOT_A, then
# BOT_B) from a safehouse save: the module is named; no wait ran out; both steps' bot waits ended with the bot stopped;
# the game left by WM_CLOSE.  Prints the steps' outcome lines (a probe: what the bot logs is the result).
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
[ "$(grep -c 'bot wait: the bot stopped' "$LOG")" -eq 2 ] || fail "want 2 bot waits ended by the bot, got $(grep -c 'bot wait: the bot stopped' "$LOG")"
grep -q 'LOG bot_probe: done' "$LOG" || fail "no 'bot_probe: done' marker"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
echo "$(grep -E -- '-> done|over:|carreset:' "$LOG" | sed 's/^\[mw[a-z]*\] f=[0-9]* //' | tr '\n' ';' | cut -c1-600)"
