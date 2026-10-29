#!/bin/bash
# Suite check for "pursuit_heat" (tests/mw/scripts/pursuit_heat.txt, TRACKER M4-16): the module under test is named
# (L-020); every wait in time; Game.ForcePursuitStart(2) raised the heat to 2.00 as the pursuit came; from then on
# the pursuit's heat stayed in its range (min(2, its maximum) once its update had run: never above the maximum
# after the first line that shows the heat back at it), and the game ended the pursuit as evaded or busted (the
# aid's line from the game's own code) before the pursuit object went; the game left by WM_CLOSE.  Prints the
# range and the end.
LOG="$1"
fail(){ echo "$1"; exit 1; }
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
miss=$(inorder 'call 0x0060aac0 -> ' 'pursuit wait on: satisfied' 'LOG pursuit_heat: started' \
  '] f=' 'pursuit wait off: satisfied' 'LOG pursuit_heat: ended') || fail "missing, or out of order: $miss"
# the watch lines with a pursuit object: heat H, range MIN-MAX
res=$(awk '/^\[mwpursuit\] f=[0-9]+ heat [0-9.]+ pursuit [0-9a-f]+ time [0-9.]+ range / && $6 != "00000000" {
    h = $4 + 0; split($10, r, "-"); mx = r[2] + 0; n++
    if (n == 1) { first = h; range = $10; next }
    if (h <= mx + 0.001) back = 1
    else if (back) { print "BAD heat " h " above the range " $10 ": " $0; exit }
  }
  END { if (!n) print "BAD no pursuit line with a range"; else if (first != 2) print "BAD the first heat " first ", not 2.00";
        else if (!back) print "BAD the heat never came back into the range " range; else print "range " range }' "$LOG")
case "$res" in BAD*) fail "${res#BAD }" ;; esac
end=$(grep -oE '^\[mwpursuit\] f=[0-9]+ (evaded|busted): ' "$LOG" | head -1 | awk '{ print $3 }' | tr -d :)
[ -n "$end" ] || fail "no evaded or busted line: the game's end of the pursuit was not seen"
miss=$(inorder 'LOG pursuit_heat: started' "] f=" "$end: pursuit" 'pursuit wait off: satisfied') || fail "the end line is not before the pursuit went: $miss"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
echo "$res; the pursuit was $end"
