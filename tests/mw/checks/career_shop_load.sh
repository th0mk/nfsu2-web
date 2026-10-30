#!/bin/bash
# Suite check for "career_shop_load" (tests/mw/scripts/career_shop_load.txt, TRACKER M4-12), a start seeded with the
# save career_shop.txt left: the module under test is named (L-020); every wait in time; the safehouse menu showed
# Cash 2,000 after the restart; the shop's spoilers screen opened on the Reaper the car was given; the shop was
# left for free roam; all in that order; the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
miss=$(inorder "waitfng MainMenu_Sub.fng'2,000 satisfied" 'LOG career_shop_load: safehouse menu, Cash 2,000' \
  'waitfng CustomizeMain.fng:055dce1a@4b4e388a satisfied' 'waitfng Spoilers.fng@4b4e388b satisfied' \
  'LOG career_shop_load: the spoiler' 'LOG career_shop_load: left the shop') || fail "missing, or out of order: $miss"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "after a restart: Cash 2,000 in the safehouse, the spoilers screen on the bought Reaper; the game left by WM_CLOSE"
