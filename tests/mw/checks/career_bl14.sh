#!/bin/bash
# Suite check for "career_bl14" (tests/mw/scripts/career_bl14.txt, TRACKER M4-13, codepdbh #162): Blacklist #14's race
# from a save with his requirements met: the module is named (L-020); no wait ran out (the first race got 15 % in,
# past its first corner) and both expects passed (bin 14 at the start, 13 at the end); the rival's challenge was
# accepted, his race ended and the screens after it came (the beaten rival's stats, the markers, the next rival's
# bio), none left as it was; the bot reached bin 13; the game left by WM_CLOSE.  Prints the player's progress at
# the 15 % mark and the markers' cards.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q '^\[expect\] FAIL' "$LOG" && fail "an expect failed: $(grep -m1 '^\[expect\] FAIL' "$LOG")"
[ "$(grep -c '^\[expect\] PASS' "$LOG")" -eq 2 ] || fail "want 2 passed expects (bin 14, then 13), got $(grep -c '^\[expect\] PASS' "$LOG")"
grep -q "the rival's challenge accepted" "$LOG" || fail "the rival's challenge was not accepted"
grep -q 'LOG career_bl14: past the first corner' "$LOG" || fail "the race did not get past its first corner"
grep -q "over: (the rival's)" "$LOG" || fail "the rival's race did not end"
order=$(grep -oE 'mwbot\] f=[0-9]+ screen (SafeHouseRivalChallenge|SafehouseMarkers|SafeHouseRivalBio)\.fng' "$LOG" \
  | sed 's/.* screen //' | awk '!s[$0]++' | tr '\n' ' ')
[ "$order" = "SafeHouseRivalChallenge.fng SafehouseMarkers.fng SafeHouseRivalBio.fng " ] || fail "the screens after the win: $order"
grep -q ': left as it is' "$LOG" && fail "a screen stuck: $(grep -m1 ': left as it is' "$LOG")"
grep -q 'reached bin 13' "$LOG" || fail "the bot did not reach bin 13: $(grep -m1 -E -- '-> done' "$LOG")"
grep -q 'LOG career_bl14: Blacklist 13' "$LOG" || fail "no 'career_bl14: Blacklist 13' marker"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
at=$(grep -A3 'waitexpr satisfied after [0-9]* frames: f:\[0x0091e000\]+0x30' "$LOG" | grep -m1 -oE 'racestatus .*' | grep -oE '#0 [0-9.]+%')
echo "Blacklist 13 reached; the rival's first race past 15 % (${at:-?}), won; stats, markers, bio in order"
