#!/bin/bash
# Suite check for "shop_music" (tests/mw/scripts/shop_music.txt, TRACKER M4-26), run with READLOG=1: the module under
# test is named (L-020); every wait in time; 50 shop visits, each engaged at the gate and left for free roam; after
# each one the game read the music again (MW_Music.mus) within its 20 s ("[status] shop_out_N" to "[status] music_N");
# the game left by WM_CLOSE.  Prints the visits, the music reads after the visits (fewest in a pass) and in the shop.
LOG="$1"
N=${SHOP_N:-50}
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q '\[read\] ' "$LOG" || fail "no [read] lines: run it with READLOG=1"
grep -q 'LOG shop_music: free roam' "$LOG" || fail "free roam never came"
[ "$(grep -c 'waitfng CustomizeMain.fng:055dce1a@4b4e388a satisfied' "$LOG")" -eq "$N" ] || fail "want $N shop visits"
[ "$(grep -c '^\[status\] shop_out_' "$LOG")" -eq "$N" ] || fail "want $N returns to free roam"
# per pass: music reads after the shop (shop_out_N .. music_N) and in it (shop_in_N .. shop_out_N)
counts=$(awk '
  /^\[status\] shop_in_/  { split($2, a, "_"); p = a[3]; where = "in" }
  /^\[status\] shop_out_/ { split($2, a, "_"); p = a[3]; where = "out" }
  /^\[status\] music_/    { where = "" }
  /\[read\] .*MW_Music\.mus/ { if (where == "out") after[p]++; else if (where == "in") inside[p]++ }
  END { min = -1; silent = ""; tin = 0
        for (i = 1; i <= '"$N"'; i++) { n = after[i] + 0; tin += inside[i]
          if (min < 0 || n < min) min = n; if (!n) silent = silent " " i }
        printf "%d %d%s\n", min, tin, silent ? " passes without music after the shop:" silent : "" }' "$LOG")
read -r min inside rest <<< "$counts"
[ -n "$rest" ] && fail "$rest"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "PASS: $N shop visits; music read again after every one (at least $min reads in each pass's 20 s); $inside music reads in the shop in all"
