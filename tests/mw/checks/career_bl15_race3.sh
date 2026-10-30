#!/bin/bash
# Suite check for "career_bl15_race3" (tests/mw/scripts/career_bl15_race3.txt, TRACKER M4-13, codepdbh #112): the
# third race of Blacklist #15's bin from a save with two won: the module is named (L-020); no wait ran out (the race
# got 15 % in) and the expect passed (bin 15); the bot engaged a race, the race went on, and the bot stopped because
# three of the bin's races were won (its last gates line says 3); the game left by WM_CLOSE.  Prints the races the
# bot engaged and how they ended.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q '^\[expect\] FAIL' "$LOG" && fail "an expect failed: $(grep -m1 '^\[expect\] FAIL' "$LOG")"
grep -qE 'gates \(asked\): bin 15 races won 2/3' "$LOG" || fail "the seed has not two of the bin's races won: $(grep -m1 'gates (asked)' "$LOG")"
grep -q 'goto -> engage: accepted' "$LOG" || fail "no race engaged"
grep -q 'engage -> race: the race is on' "$LOG" || fail "the race did not begin"
grep -q 'LOG career_bl15_race3: past the first corner' "$LOG" || fail "the race did not get past its first corner"
grep -qF -- '-> done: the races asked for are won' "$LOG" || fail "the bot stopped for something else: $(grep -m1 -E -- '-> done' "$LOG")"
grep -E 'gates \(asked\)' "$LOG" | tail -1 | grep -q 'races won 3/3' || fail "not three races won at the end"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "the third race won; races: $(grep -oE 'race [0-9]+ over: [a-z ]+' "$LOG" | tr '\n' ';')"
