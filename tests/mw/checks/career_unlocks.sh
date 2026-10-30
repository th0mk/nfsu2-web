#!/bin/bash
# Suite check for "career_unlocks" (tests/mw/scripts/career_unlocks.txt, TRACKER M4-44), a probe run with
# NFSU2_FNG_LOG=3: the module is named; no wait ran out; both track lists were shown to their end.  Prints how many of
# each kind's tracks the list offers (a track offered draws the Accept button, label 0x2B65A216; a locked one draws
# none) out of how many: the Quick Race tracks this save's career has unlocked.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q 'LOG career_unlocks: done' "$LOG" || fail "no 'career_unlocks: done' marker"
count(){   # the kind's title label: offered/total over the distinct track numbers shown
  grep -E "Track_Select.fng shows .*:$1'" "$LOG" | awk -v acc=':2b65a216' '{
    n = 0; idx = ""; tot = "";
    for (i = 1; i <= NF; i++) if ($i ~ /^:00000018\x27/) { n++; v = substr($i, 11); if (n == 3) idx = v; if (n == 4) tot = v }
    if (idx == "") next; seen[idx] = 1; if (index($0, acc)) ok[idx] = 1; t = tot }
    END { o = 0; s = 0; for (k in seen) { s++; if (k in ok) o++ } printf "%d of %s offered (%d shown)", o, t, s }'
}
c=$(count 3de80a85); s=$(count c2d85652)
case "$c$s" in *"(0 shown)"*) fail "a track list was not shown: circuits $c, sprints $s" ;; esac
echo "circuits $c; sprints $s"
