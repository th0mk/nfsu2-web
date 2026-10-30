#!/bin/bash
# Suite check for "career_bl15" (tests/mw/scripts/career_bl15.txt, TRACKER M4-13, codepdbh #84): Blacklist #15 beaten
# from a save with his requirements met: the module is named (L-020); no wait ran out and both expects passed (bin 15
# at the start, 14 at the end: the counter moved on); the rival's challenge was accepted and his race ended; after it
# the beaten rival's stats (SafeHouseRivalChallenge.fng), the markers (SafehouseMarkers.fng, two taken) and the next
# rival's bio (SafeHouseRivalBio.fng) came in that order and none was left as it was (the stats screen did not
# stick); the bot reached bin 14; the game left by WM_CLOSE.  Prints the markers' cards as the screen showed them.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q '^\[expect\] FAIL' "$LOG" && fail "an expect failed: $(grep -m1 '^\[expect\] FAIL' "$LOG")"
[ "$(grep -c '^\[expect\] PASS' "$LOG")" -eq 2 ] || fail "want 2 passed expects (bin 15, then 14), got $(grep -c '^\[expect\] PASS' "$LOG")"
grep -q "the rival's challenge accepted" "$LOG" || fail "the rival's challenge was not accepted"
grep -q "over: (the rival's)" "$LOG" || fail "the rival's race did not end"
order=$(grep -oE 'mwbot\] f=[0-9]+ screen (SafeHouseRivalChallenge|SafehouseMarkers|SafeHouseRivalBio)\.fng' "$LOG" \
  | sed 's/.* screen //' | awk '!s[$0]++' | tr '\n' ' ')
[ "$order" = "SafeHouseRivalChallenge.fng SafehouseMarkers.fng SafeHouseRivalBio.fng " ] || fail "the screens after the win: $order"
grep -q ': left as it is' "$LOG" && fail "a screen stuck: $(grep -m1 ': left as it is' "$LOG")"
[ "$(grep -c 'SafehouseMarkers.fng @[0-9a-f]*: Enter: take' "$LOG")" -ge 2 ] || fail "fewer than two markers taken"
grep -q 'reached bin 14' "$LOG" || fail "the bot did not reach bin 14: $(grep -m1 -E -- '-> done' "$LOG")"
grep -q 'LOG career_bl15: Blacklist 14' "$LOG" || fail "no 'career_bl15: Blacklist 14' marker"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
cards=$(grep -E 'SafehouseMarkers.fng shows' "$LOG" | tail -1 | grep -oE "'[A-Za-z_]*(Pink|Slip|Cash|Impound|Jail|Release|Part|Upgrade|Marker)[A-Za-z_,0-9]*" | sort -u | tr '\n' ' ')
echo "Blacklist 14 reached; the rival's race won, stats, markers, bio in order; cards: ${cards:-not read}"
