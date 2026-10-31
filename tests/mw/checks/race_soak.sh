#!/bin/bash
# Suite check for "race_soak" (tests/mw/scripts/race_soak.txt, G-03, TRACKER M4-08): the module under test is named
# (L-020); every wait in time; every cycle (SOAK_N of them when the environment names it, else all the loads seen,
# at least 2) loaded, reached GO and quit through the pause menu; no stall report; and over the last half of the
# cycles, from the [status] line before each load (in the "quick race" menu), the guest heap in use and the
# textures' MB (mtl) stay within 5 % of their mean (L-054, G-03), and the texture ids (tex), the kernel objects in
# use less the ended threads (`objects`: the sound thread 0x820BD7 made at each load ends, and the game never closes
# its handle, GAME 2.13) and each game memory pool's allocated KB (`pools`) do not grow from the third quarter of the
# cycles to the last (the last quarter's mean at most 5 % and 2 above the third quarter's), all but the Cars pool,
# the car loader's cache of the cars the random races picked (18 to 34 MB of its 50 over 100 cycles), which is printed
# only; the game left by WM_CLOSE.  Prints the first and last values, the peaks (the heap's peak, each pool's peak
# since `pools watch`) and the load times.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
N=$(grep -c 'LOG race_soak: load$' "$LOG")
[ -n "$SOAK_N" ] && [ "$N" != "$SOAK_N" ] && fail "$N of $SOAK_N cycles loaded"
[ "$N" -ge 2 ] || fail "only $N cycles"
for m in racing GO quit; do
  n=$(grep -c "LOG race_soak: $m\$" "$LOG")
  [ "$n" = "$N" ] || fail "$n of $N cycles reached '$m'"
done
grep -qF '[watchdog] no Present' "$LOG" && fail "a stall: $(grep -m1 -F '[watchdog] no Present' "$LOG")"
grep -q 'LOG race_soak: done$' "$LOG" || fail "no 'done' marker"
out=$(awk -v N="$N" '
  function num(s, k,   i, a) { if (!match(s, k "=[0-9.]+")) return -1; return substr(s, RSTART + length(k) + 1, RLENGTH - length(k) - 1) + 0 }
  function sample(name, v) { if (v < 0) return; c = cnt[name]++; val[name, c] = v; if (!(name in order)) { order[name] = no++; names[no - 1] = name } }
  /^\[status\] soak(_[0-9]+)? / { st++; inpool = 1
    if (match($0, /heap=[0-9.]+\/[0-9.]+MB/)) { split(substr($0, RSTART + 5, RLENGTH - 7), h, "/"); sample("heap MB", h[1] + 0); hpeak = h[2] }
    sample("textures MB", num($0, "mtl")); sample("texture ids", num($0, "tex"))
    next }
  /^\[objects\] f=[0-9]+ [0-9]+ in use/ && inpool {   # less the ended threads, whose handles the game never closes
    match($0, /[0-9]+ in use/); o = substr($0, RSTART, RLENGTH) + 0; e = 0
    while (match($0, /\([0-9]+ ended\)/)) { e += substr($0, RSTART + 1, RLENGTH - 8) + 0; $0 = substr($0, RSTART + RLENGTH) }
    sample("kernel objects (ended threads aside)", o - e); next }
  /^\[mwpool\] f=[0-9]+ pool [0-9]+ "/ && inpool {
    match($0, /pool [0-9]+ "[^"]*"/); p = substr($0, RSTART, RLENGTH)
    match($0, /allocated [0-9]+ KB/); a = substr($0, RSTART + 10, RLENGTH - 13) + 0
    match($0, /peak [0-9]+ KB in [0-9]+/); peak[p] = substr($0, RSTART + 5, RLENGTH - 5)
    sample(p " KB", a) }
  /LOG race_soak: (load|racing)$/ { if (!match($0, /t=[0-9]+ms/)) next; t = substr($0, RSTART + 2, RLENGTH - 4) / 1000
    if ($NF == "load") tl = t; else { d = t - tl; ls += d; ln++; if (d > lmax) lmax = d } }
  /LOG race_soak: (racing|quit)$/ { inpool = 0 }
  END {
    bad = 0
    for (j = 0; j < no; j++) { k = names[j]; n = cnt[k]; from = int(n / 2); s = 0; mn = 1e18; mx = -1
      for (i = from; i < n; i++) { v = val[k, i]; s += v; if (v < mn) mn = v; if (v > mx) mx = v }
      m = s / (n - from); flag = ""
      if (k == "heap MB" || k == "textures MB") { if (m > 0 && (mx > m * 1.05 || mn < m * 0.95)) flag = " NOT FLAT" }
      else if (k !~ /"Cars"/) { s3 = 0; n3 = 0; s4 = 0; n4 = 0     # the rest: no growth of the mean from the third quarter to the last
        for (i = int(n / 2); i < int(3 * n / 4); i++) { s3 += val[k, i]; n3++ }
        for (i = int(3 * n / 4); i < n; i++) { s4 += val[k, i]; n4++ }
        if (n3 && n4 && s4 / n4 > s3 / n3 * 1.05 + 2) flag = sprintf(" GROWS (%g -> %g)", s3 / n3, s4 / n4) }
      if (flag != "") bad++
      printf "%s %g -> %g (last half %g..%g)%s; ", k, val[k, 0], val[k, n - 1], mn, mx, flag }
    printf "heap peak %s MB; pool peaks:", hpeak; for (p in peak) printf " %s %s", p, peak[p]
    printf "; loads %.1f s mean, %.1f s max over %d; status lines %d\n", ln ? ls / ln : 0, lmax, ln, st
    print "notflat " bad }' "$LOG")
echo "$out" | grep -q '^notflat 0$' || fail "not flat, or growing, over the last half of $N cycles: $(echo "$out" | head -1)"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "$N cycles of load, race and quit; flat over the last half: $(echo "$out" | head -1)"
