#!/bin/bash
# Check for one page of the "roam_mp_spawn" pair (tests/mw/scripts/roam_mp_spawn_a.txt and _b.txt, web/tools/roam-pair.mjs
# --game mw; TRACKER M10-15's spawn rule, M10-07's NaN poke), run on each page's log (roam-pair's <name>.log: its lines
# carry a time and a thread before the game's text; roam-pair's summary names the module): the career saved in free
# roam loaded into free roam; the friend's car came as a ghost; the player's car, standing, moved no more than 0.05 m
# in the frames after it came (with solid cars both on the one spot it was shoved 0.6 to 0.9 m); the friend's car went
# solid once clear of every other car; where the page poked NaN into the friend's car's wheels and suspension (`roam mp
# nanpoke`), the next frames put every field back and the car stayed (listed, its four wheels finite); the game left by
# WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q 'LOG roam_mp_spawn: free roam' "$LOG" || fail "the career did not load into free roam"
# (the friend's car may come before the script's own free-roam line: its 300 frames)
miss=$(inorder "'s car: " 'the player'"'"'s car moved' 'is clear of every other car' 'LOG roam_mp_spawn: done') \
  || fail "missing, or out of order: $miss"
grep -qE "\[roam\] f=[0-9]+ [^ ]+'s car: [a-z0-9]+ \(key [0-9a-f]{8}\), .*, a ghost;" "$LOG" || fail "the friend's car did not come as a ghost"
moved=$(sed -nE "s/.*\[roam\] f=[0-9]+ the player's car moved ([0-9.]+) m in the [0-9]+ frames since .*/\1/p" "$LOG" | head -1)
awk -v m="$moved" 'BEGIN { exit !(m != "" && m + 0 <= 0.05) }' || fail "the player's car moved $moved m after the friend's car came (want 0.05 m or less)"
nan=""
if grep -q '\[roam\] f=[0-9]* nanpoke: ' "$LOG"; then
  poked=$(sed -nE 's/.*\[roam\] f=[0-9]+ nanpoke: ([0-9]+) fields of .*/\1/p' "$LOG" | head -1)
  [ "${poked:-0}" -gt 0 ] || fail "the poke reached no field: $(grep -m1 'nanpoke: ' "$LOG")"
  miss=$(inorder ' nanpoke: ' 'wheel and suspension fields not finite: put back' 'wheels finite 4 of 4' 'LOG roam_mp_spawn: done') \
    || fail "after the NaN poke, missing or out of order: $miss"
  after=$(sed -n '/ nanpoke: /,$p' "$LOG")
  grep -qE "car [0-9]+: .* listed 1, .*wheels finite 4 of 4" <<<"$after" || fail "after the poke no status line showed the car listed with four finite wheels"
  put=$(sed -nE "s/.*'s car: ([0-9]+) wheel and suspension fields not finite: put back.*/\1/p" <<<"$after" | head -1)
  nan=", NaN poked into $poked fields: the four wheels finite again the next frames ($put fields put back, the game wrote the others)"
fi
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "the friend's car came as a ghost, the player's car moved $moved m, then $(grep -m1 -oE 'clear of every other car \([-0-9.]+ m\): solid after [0-9]+ frames' "$LOG")$nan"
