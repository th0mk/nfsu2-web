#!/bin/bash
# Suite check for "career_bot" (tests/mw/scripts/career_bot.txt, TRACKER M4-15), a start seeded with a safehouse save:
# the module under test is named (L-020); no wait ran out and the one expect passed (the career at the bin the run
# asked for, BOT_BIN); the bot stopped because it reached that bin (its "reached bin" line), not for anything it could
# not do; it saved at the safehouse (the script's 'saved' marker after the bot's "saved"); the game left by WM_CLOSE.
# Prints what the bot did: races won and lost, pursuits, wedge resets, the gates at the end.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q '^\[expect\] FAIL' "$LOG" && fail "an expect failed: $(grep -m1 '^\[expect\] FAIL' "$LOG")"
[ "$(grep -c '^\[expect\] PASS' "$LOG")" -eq 1 ] || fail "want 1 passed expect (the bin), got $(grep -c '^\[expect\] PASS' "$LOG")"
grep -q '\[mwbot\] .* reached bin ' "$LOG" || fail "the bot did not reach the bin: $(grep -m1 -E '\[mwbot\] .* -> done' "$LOG")"
grep -qF -- '-> done: saved' "$LOG" || fail "the bot did not save"
grep -q 'LOG career_bot: saved' "$LOG" || fail "no 'career_bot: saved' marker"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
won=$(grep -c ' over: completed' "$LOG"); lost=$(grep -c ' over: not completed' "$LOG")
rival=$(grep -c "over: (the rival's)" "$LOG"); pur=$(grep -c 'pursuit over after' "$LOG"); rs=$(grep -c 'wedged; reset' "$LOG")
echo "reached bin $(grep -m1 -oE 'reached bin [0-9]+' "$LOG" | grep -oE '[0-9]+$'), saved; races completed $won, not $lost;" \
  "the rival's races $rival; pursuits $pur; wedge resets $rs; $(grep -E '\[mwbot\] .* gates \(asked\)' "$LOG" | tail -1 | sed 's/.*gates (asked): //')"
