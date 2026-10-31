#!/bin/bash
# Check for one page of the "roam_mp_jump" pair (tests/mw/scripts/roam_mp_jump_a.txt and _b.txt, web/tools/roam-pair.mjs
# --game mw; TRACKER M10-15's jump rule), run on each page's log (roam-pair's <name>.log).  Both pages: the career saved
# in free roam loaded into free roam, the friend's car came as a ghost, it went solid once clear, and after the warp
# back onto the spot it went solid again once A had driven off; the game left by WM_CLOSE.  A's page (it warps): its
# car jumped twice and only then (no jump while the game's AI drove it), the warp back made B's car a ghost here, and
# its car moved no more than 0.15 m in the 120 frames after it landed (settling on the road; shoved by a solid car it
# moves 0.6 m and more).  B's page (it stands): A's car was a ghost again for a new teleport epoch, B's own car never
# jumped, and no step of its `where` series moved it more than 0.05 m, nor did the 120 frames after A's car jumped.
LOG="$1"
fail(){ echo "$1"; exit 1; }
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q 'LOG roam_mp_jump: free roam' "$LOG" || fail "the career did not load into free roam"
grep -qE "\[roam\] f=[0-9]+ [^ ]+'s car: [a-z0-9]+ \(key [0-9a-f]{8}\), .*, a ghost;" "$LOG" || fail "the friend's car did not come as a ghost"
jumps=$(grep -c '\[roam\] f=[0-9]* my car jumped ' "$LOG")
if grep -q 'LOG roam_mp_jump: back' "$LOG"; then
  [ "$jumps" -eq 2 ] || fail "my car jumped $jumps times (want 2: the two warps)"
  miss=$(inorder 'is clear of every other car' 'LOG roam_mp_jump: back' 'my car jumped ' 'since its jump' \
         'is clear of every other car' 'LOG roam_mp_jump: done') || fail "missing, or out of order: $miss"
  back=$(sed -n '/LOG roam_mp_jump: back/,$p' "$LOG")
  grep -qE "my car jumped [0-9.]+ m .*: a new teleport epoch; 1 friends' cars here ghosts until clear" <<<"$back" \
    || fail "the warp back made no friend's car here a ghost: $(grep -m1 -oE 'my car jumped .*' <<<"$back")"
  settle=$(sed -nE "s/.*\[roam\] f=[0-9]+ the player's car moved ([0-9.]+) m in the [0-9]+ frames since its jump.*/\1/p" <<<"$back" | head -1)
  awk -v m="$settle" 'BEGIN { exit !(m != "" && m + 0 <= 0.15) }' || fail "after the warp back the player's car moved $settle m (want 0.15 m or less)"
  what="the warp back: $(grep -m1 -oE 'my car jumped [0-9.]+ m' <<<"$back"), the friend's car a ghost here, the player's car moved $settle m"
else
  [ "$jumps" -eq 0 ] || fail "the standing car jumped $jumps times"
  # (A's car goes solid after its first warp about when the series starts, and again once A drove off, which may be
  # before the series ends; a ghost again it was solid)
  miss=$(inorder 'LOG roam_mp_jump: standing' "'s car jumped " 'is clear of every other car' 'LOG roam_mp_jump: done') \
    || fail "missing, or out of order: $miss"
  miss=$(inorder 'LOG roam_mp_jump: standing' "'s car jumped " 'LOG roam_mp_jump: stood') || fail "the jump was not while standing: $miss"
  grep -qE "'s car jumped [0-9.]+ m \(a new teleport epoch\): a ghost until clear" "$LOG" || fail "no friend's car was a ghost again for a new teleport epoch"
  worst=$(sed -nE "s/.*the player's car moved ([0-9.]+) m in the [0-9]+ frames since [^ ]+'s car jumped.*/\1/p" "$LOG" | sort -g | tail -1)
  awk -v m="$worst" 'BEGIN { exit !(m != "" && m + 0 <= 0.05) }' || fail "after the friend's car jumped the player's car moved $worst m (want 0.05 m or less)"
  series=$(sed -n '/LOG roam_mp_jump: standing/,/LOG roam_mp_jump: stood/p' "$LOG" | sed -nE 's/.*\[mwrace\] f=[0-9]+ where ([-0-9.]+) ([-0-9.]+) ([-0-9.]+) .*/\1 \2 \3/p')
  n=$(wc -l <<<"$series")
  [ "$n" -ge 25 ] || fail "only $n where lines while standing"
  step=$(awk 'NR == 1 { x = $1; y = $2; z = $3 } { d = sqrt(($1-x)^2 + ($2-y)^2 + ($3-z)^2); if (d > m) m = d } END { printf "%.2f", m }' <<<"$series")
  awk -v m="$step" 'BEGIN { exit !(m + 0 <= 0.05) }' || fail "while standing the player's car went $step m from where it stood (want 0.05 m or less)"
  what="the friend's car a ghost again for a new teleport epoch, the player's car moved $worst m after it and at most $step m over $n where lines"
fi
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "$what"
