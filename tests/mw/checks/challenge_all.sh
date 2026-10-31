#!/bin/bash
# Suite check for "challenge_all" (tests/mw/scripts/challenge_all.txt, TRACKER M4-21): the module under test is named (L-020);
# every wait in time; each of the 69 events launched, reached GO, was won by the aid
# (challengewin: Game.ChallengeComplete set the racer's goal byte, then racewin's EnterPostRaceFlow with the race
# won; the race's EventID is the list's event's: the bin's in order, from the first `challenges` line, but the list's
# 69th is the bin's 70th, the Black Edition's 19.9.70, as the list leaves out the bin's 69th, 19.8.31), its end
# screen came (NFSU2_FNG_LOG=1: InGamePhotoMaster.fng or PostRace_MilestoneRewards.fng), and the
# `challenges` line after it shows that event's flag 1 (won); ALPHA was
# written after the last one, and saved; the game left by WM_CLOSE.  Prints the events by type and the cars.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
N=challenge_all EVENTS=$(seq 1 69)
out=$(awk -v N="$N" -v want="$(echo $EVENTS)" '
  BEGIN { split(want, W, " "); for (i in W) need[W[i]] = 1 }
  /^\[mwchal\]   events/ && !have { have = 1; n = split($0, F, " "); for (i = 1; i <= n; i++) if (F[i] ~ /^[0-9]+:[0-9a-f]+=/) { split(F[i], G, /[:=]/); bid[G[1]] = G[2] } }
  $0 ~ "LOG " N ": event [0-9]+ GO$" { ev = $(NF - 1); bi = ev == 69 ? 70 : ev; st = 1; next }
  st == 1 && /raceinfo type/ { match($0, /type [0-9]+/); ty[ev] = substr($0, RSTART + 5, RLENGTH - 5); match($0, /event [0-9a-f]+/); if (substr($0, RSTART + 6, RLENGTH - 6) != bid[bi]) wrongev = wrongev " " ev; match($0, /car [^;]+/); car[ev] = substr($0, RSTART + 4, RLENGTH - 4); next }
  st == 1 && /\[mwchal\] f=[0-9]+ challengewin: Game.ChallengeComplete\(racer 1\): goal byte 1/ { st = 2; next }
  st == 2 && /racewin: racer 1 finished .*EnterPostRaceFlow/ { st = 3; next }
  st == 3 && /racestatus flow 6 .* won 1 player 1:/ { st = 4; next }
  st == 4 && $0 ~ "LOG " N ": event " ev " results$" { st = 5; next }
  st >= 1 && /^\[fng\] f=[0-9]+ screen (InGamePhotoMaster|PostRace_MilestoneRewards)\.fng/ { match($0, /screen [A-Za-z_]+/); scr[ev] = substr($0, RSTART + 7, RLENGTH - 7); next }
  st == 5 && /^\[mwchal\]   events/ { if (match($0, " " bi ":[0-9a-f]+=[0-9a-f]+")) { f = substr($0, RSTART, RLENGTH); sub(/.*=/, "", f)
      if (f ~ /[13579bdf]$/ && scr[ev] != "") won[ev] = 1 } st = 0 }
  END { bad = ""; for (e in need) if (!won[e]) bad = bad " " e
    for (e in need) if (won[e]) { k[ty[e] " " scr[e]]++; c[car[e]]++ }
    nw = 0; for (e in need) if (won[e] == 1) nw++; nn = 0; for (e in need) nn++; printf "won %d of %d;", nw, nn; for (t in k) printf " type %s: %d", t, k[t]; printf "; cars:"; for (x in c) printf " %s %d", x, c[x]
    printf "\nmissing%s%s\n", bad, wrongev != "" ? "; not the list event (its EventID):" wrongev : "" }' "$LOG")
echo "$out" | grep -q '^missing$' || fail "not won through the aid with its flag set: $(echo "$out" | tr '\n' '|')"
W="[write] CreateFileA(CREATE_ALWAYS, 0x40000000) \"C:\NFSMW\NFS Most Wanted/ALPHA/ALPHA\""
L=$(grep -n "LOG $N: event [0-9]* results\$" "$LOG" | tail -1 | cut -d: -f1)
tail -n +"$L" "$LOG" | grep -qF "$W" || fail "ALPHA was not written after the last event"
grep -q "LOG $N: ALPHA saved\$" "$LOG" || fail "no save"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "Challenge Series through the aid, saved: $(echo "$out" | head -1)"
