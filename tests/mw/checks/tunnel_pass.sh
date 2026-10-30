#!/bin/bash
# Suite check for "tunnel_pass" (tests/mw/scripts/tunnel_pass.txt, TRACKER M4-27), run with NFSMW_TUNNEL_LOG=1: the
# module under test is named (L-020); every wait in time; 20 passes driven; the player's car went into the tunnel in
# each, and out of it again after every entry, by road or at the next warp (runtime/mw/sound_mw.c: "[mw-sound] ...
# tunnel in/out", the game's own sound-zone state; the player's lines are those without "no reverb change", where
# the game switches the reverb); the sound thread never stalled; the game left by WM_CLOSE (a freeze would have run
# into the time limit instead).  Prints the passes, how many left by road, and the zone types seen.
LOG="$1"
N=${TUNNEL_N:-20}
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q 'LOG tunnel_pass: free roam' "$LOG" || fail "free roam never came"
grep -q 'DirectSound STALL' "$LOG" && fail "the sound thread stalled: $(grep -m1 'DirectSound STALL' "$LOG")"
# the player's in/out lines alternate; each pass (ended by its [status] pass_N line) has an entry; an exit before
# the pass's end is one by road
res=$(awk '
  /^\[mw-sound\] f=[0-9]+ tunnel in: / && !/no reverb change/ { if (inside) twice++; inside = 1; ins++; got = 1 }
  /^\[mw-sound\] f=[0-9]+ tunnel out: / && !/no reverb change/ { if (inside) { outs++; if (got) road++ } inside = 0 }
  /^\[status\] pass_/ { p++; if (!got) miss = miss " " p; got = 0 }
  END { printf "%d %d %d %d %d%s\n", p, ins, outs, road, twice, miss ? " passes without an entry:" miss : "" }' "$LOG")
read -r passes ins outs road twice rest <<< "$res"
[ "$passes" -eq "$N" ] || fail "want $N passes, $passes ran"
[ -n "$rest" ] && fail "$rest"
[ "$twice" -eq 0 ] || fail "$twice entries without an exit between them"
[ "$outs" -eq "$ins" ] || fail "$ins entries but $outs exits"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
types=$(grep -E 'tunnel in: zone type' "$LOG" | grep -v 'no reverb change' | grep -oE 'zone type [0-9]+' | awk '{print $3}' | sort -u | tr '\n' ' ')
echo "PASS: $N passes, $ins tunnel entries and as many exits ($road by road); zone types ${types% }; no sound stall"
