#!/bin/bash
# Suite check for "pursuit_rap" (tests/mw/scripts/pursuit_rap.txt; TRACKER M4-16, M4-17, M4-18, M4-28, M4-35): the
# module under test is named (L-020); no wait ran out; the rap sheet was logged at the safehouse, after the pursuits and
# after the save; the bot ran at least one pursuit to its end, stopped by itself (not at game over) and saved; the game
# left by WM_CLOSE.  Prints what the pursuits showed: how many, busted and evaded, the highest heat, cops, helicopters,
# roadblocks (spawned, dodged), spike strips dodged, pursuit breakers, the purecall guard's runs, and the rap sheet
# before and after.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
[ "$(grep -c '^\[mwrap\] f=[0-9]* rapsheet (asked)' "$LOG")" -ge 3 ] || fail "want 3 rap sheets asked (safehouse, after the pursuits, after the save)"
grep -q '\[mwbot\] .* pursuit over (' "$LOG" || fail "no pursuit came to its end"
grep -q 'screen GameOver.fng' "$LOG" && fail "game over: $(grep -m1 -E 'bot stops' "$LOG")"
grep -qF -- '-> done: saved' "$LOG" || fail "the bot did not save"
grep -q 'LOG pursuit_rap: saved' "$LOG" || fail "no 'pursuit_rap: saved' marker"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
last=$(grep -E '\[mwbot\] .* pursuit over \(' "$LOG" | tail -1 | grep -oE 'pursuits [0-9]+, busted [0-9]+, evaded [0-9]+')
# the highest value of a field of the [mwpursuit] lines while a pursuit is on (a word of its own: " cops" is the
# pursuit's count, not "total_cops")
max(){ grep -E '^\[mwpursuit\] f=[0-9]+ heat .* pursuit [0-9a-f]*[1-9a-f][0-9a-f]* ' "$LOG" | grep -oE " $1 [0-9.]+" |
  awk '{ if ($2 + 0 > m) m = $2 + 0 } END { print m + 0 }'; }
rb=$(grep -E '^\[mwpursuit\] f=[0-9]+ heat ' "$LOG" | tail -1 | grep -oE 'roadblocks [0-9]+ breakers [0-9]+ guard [0-9]+')
rap(){ grep -E '^\[mwrap\] f=[0-9]+ rapsheet \(asked\)' "$LOG" | sed -n "$1p" | sed 's/.*(asked)://'; }
# each career car's impound strikes of max (its record's bytes 3 and 2) and the cash, at the first and the last rap sheet
strikes(){ grep -E '^\[mwrap\] f=[0-9]+   (car [0-9]+ |cash )' "$LOG" | awk -v which="$1" '
  BEGIN { blk = 0 }
  function hex(x) { return index("0123456789abcdef", substr(x, 1, 1)) * 16 + index("0123456789abcdef", substr(x, 2, 1)) - 17 }
  / cash / { if (seen_car) { blk++; seen_car = 0 } c[blk] = $4; sub(/,/, "", c[blk]) }
  / career record / { seen_car = 1; h = $10; s[blk] = s[blk] " " hex(substr(h, 7, 2)) "/" hex(substr(h, 5, 2)) }
  END { b = which == "first" ? 0 : blk; printf "strikes%s, cash %s", s[b], c[b] }'; }
echo "$last; heat up to $(max heat), cops $(max cops), helis $(max helis), roadblocks dodged $(max rb_dodged)," \
  "spikes dodged $(max spikes_dodged), cops destroyed $(max destroyed); hooks: $rb; at the start $(strikes first)," \
  "at the end $(strikes last); rap sheet before:$(rap 1); after:$(rap 3)"
