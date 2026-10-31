#!/bin/bash
# Suite check for "pad_profile_load" (tests/mw/scripts/pad_profile_load.txt, TRACKER M6-37): the module under test is
# named (L-020); every wait in time; runtime/mw/pad_mw.c wrote its layout at the input set-up and once more at PADA's
# load, for a profile with the game's own layout (NFSU2_PAD_LOG=1), and not after; the game's lookup did not run at
# a load (the saved pad is the one plugged in); the table peeked after PADA's load holds the modern layout (as
# pad_binds.sh wants it) and after PADB's the rows PADB was saved with: the game's own layout with the gas on Y.
LOG="$1"
fail(){ echo "$1"; exit 1; }
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -qE '^\[autotest\] f=[0-9]+ .* TIMEOUT' "$LOG" && fail "a wait of the script ran out: $(grep -m1 -E 'TIMEOUT' "$LOG")"
grep -qF 'LOG pad_profile_load: done' "$LOG" || fail "no 'LOG pad_profile_load: done' line"
OWN="[pad] Most Wanted's default pad layout (a profile with the game's own layout)"
miss=$(inorder "[pad] Most Wanted's default pad layout (the input set-up)" "$OWN" \
  'LOG pad_profile_load: PADA loaded' 'LOG pad_profile_load: PADB loaded') || fail "missing, or out of order: $miss"
n=$(grep -cF "$OWN" "$LOG"); [ "$n" = 1 ] || fail "$n lines '$OWN', want 1 (PADA's load)"
grep -qF "[pad] Most Wanted's default pad layout (the game's lookup)" "$LOG" && fail "the game's lookup ran at a load"
# rows: phase 1 after PADA's load, 2 after PADB's; "row type code" (the second binding none, as pad_binds.sh)
A="0 7 2  1 5 2  2 4 0  3 5 0  4 2 5  5 2 11  6 2 4  7 2 0  8 2 1  9 2 8  10 3 0  15 2 3  16 2 2  18 2 9"
B="0 2 3  1 2 2  2 4 0  3 5 0  4 2 1  5 2 3  6 2 6  7 0 0  8 0 0  9 2 5  10 3 0  15 0 0  16 2 4  18 2 7"
bad=$(awk -v wa="$A" -v wb="$B" '
  index($0, "LOG pad_profile_load: PADA loaded") { ph = 1 }
  index($0, "LOG pad_profile_load: PADB loaded") { ph = 2 }
  ph && / peek \[0x[0-9a-fA-F]+\] = 0x/ { a = $0; sub(/.* peek \[/, "", a); v = a; a = substr(a, 1, 10); sub(/.*\] = /, "", v); v = substr(v, 1, 10)
                                          m[ph, strtonum(a)] = strtonum(v) }
  END { out = ""
        for (ph = 1; ph <= 2; ph++) {
          n = split(ph == 1 ? wa : wb, w, " +")
          for (i = 1; i + 2 <= n; i += 3) {
            r = w[i]; at = 9565208 + r * 52 + 28            # 0x91F418 + row * 0x34 + 0x1C
            if (!((ph, at) in m) || !((ph, at + 12) in m)) { out = out sprintf(" %s row %d: not peeked", ph == 1 ? "PADA" : "PADB", r); continue }
            if (m[ph, at] != w[i + 1] || m[ph, at + 4] != 0 || m[ph, at + 8] != w[i + 2] || (r != 10 && m[ph, at + 12] != 0))
              out = out sprintf(" %s row %d: (%d,%d,%d) second type %d, want (%d,0,%d)", ph == 1 ? "PADA" : "PADB", r,
                                m[ph, at], m[ph, at + 4], m[ph, at + 8], m[ph, at + 12], w[i + 1], w[i + 2])
          }
        }
        print out }' "$LOG")
[ -n "$bad" ] && fail "binding table:$bad"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
echo "PADA (the game's own pad layout) loaded with the modern one written over it; PADB (its own gas) kept as saved"
