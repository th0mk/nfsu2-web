#!/bin/bash
# Suite check for "race_win" (tests/mw/scripts/race_win.txt, TRACKER M4-02): the module under test is named
# (L-020); the start's Load list read the seeded ALPHA (FILELIST=1's [file] lines); the race reached GO; the
# player's racer finished over the line with the game's AI driving (no racewin shortcut in the run); the winner's
# post-race screen came, and the race status then says the race won, the player racer 1 (Game.GetRacerIndex) and
# slot #0 finished; and the Alias Manager's Save wrote ALPHA's save after the win, all in that order; the game left
# by WM_CLOSE.  Prints the player's race time.  What the save holds is compared with the seed's outside the run
# (--save-out).
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -qE '^\[mwrace\] f=[0-9]+ racewin' "$LOG" && fail "racewin finished the race, not the line: $(grep -m1 -E '^\[mwrace\] f=[0-9]+ racewin' "$LOG")"
D='C:\NFSMW\NFS Most Wanted'
R="[file] CreateFileA(\"$D/ALPHA/ALPHA\") -> " W="[write] CreateFileA(CREATE_ALWAYS, 0x40000000) \"$D/ALPHA/ALPHA\"" awk 'BEGIN { r = ENVIRON["R"]; w = ENVIRON["W"] }
  index($0, r) && !k { k = 1 }
  /LOG race_win: ALPHA loaded$/ && k == 1 { k = 2 }
  /LOG race_win: GO$/ && k == 2 { k = 3 }
  /LOG race_win: finished$/ && k == 3 { k = 4 }
  /LOG race_win: won$/ && k == 4 { k = 5 }
  /\[mwrace\] f=[0-9]+ racestatus flow 6 racers [0-9]+ active [01] won 1 player 1: #0 100\.0% finished/ && k == 5 { k = 6 }
  index($0, w) && k == 6 { k = 7 }
  /LOG race_win: ALPHA saved$/ && k == 7 { k = 8 }
  END { exit k != 8 }' "$LOG" || fail "not in order: ALPHA read, loaded, GO, the player over the line, the winner's screen, the race won by racer 1, ALPHA written, saved"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
t=$(grep -E '^\[mwrace\] f=[0-9]+ racestatus .*won 1 player 1: #0 100\.0% finished' "$LOG" | tail -1 | sed -E 's/.*: #0 100\.0% finished ([0-9.]+)s.*/\1/')
echo "ALPHA loaded, the quick race won over the line with the AI driving (the player's race time ${t} s), the winner's screen, ALPHA saved after it; the game left by WM_CLOSE"
