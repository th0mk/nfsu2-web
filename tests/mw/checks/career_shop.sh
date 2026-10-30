#!/bin/bash
# Suite check for "career_shop" (tests/mw/scripts/career_shop.txt, TRACKER M4-12), a start seeded with the save
# career_safehouse.txt left: the module under test is named (L-020); every wait in time; the shop engaged at its gate;
# every category of Parts (5), Performance (7) and Visual (7) opened a screen of its own (Rim Paint's dialog
# counts) and closed back to the category; the Reaper spoiler went into the cart, the cart and its purchase dialog
# came, the funds fell to 2,000; the shop was left for free roam; Jump to Safe House showed Cash 2,000 and the save
# completed; all in that order; the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
cats=()
for i in 0 1 2 3 4; do cats+=("[status] parts_$i "); done
for i in 0 1 2 3 4 5 6; do cats+=("[status] performance_$i "); done
for i in 0 1 2 3 4 5 6; do cats+=("[status] visual_$i "); done
miss=$(inorder 'LOG career_shop: free roam' 'waitfng CustomizeMain.fng:055dce1a@4b4e388a satisfied' 'LOG career_shop: the shop' \
  "${cats[@]}" 'LOG career_shop: every category' 'waitfng Spoilers.fng@4b4e388b satisfied' 'LOG career_shop: in the cart' \
  'waitfng ShoppingCart.fng@a3bcbd06 satisfied' 'waitfng Dialog.fng@b8a7c6cc satisfied' \
  'waitfng CustomizeMain.fng:055dce1a@4b4e388a satisfied' 'LOG career_shop: bought' 'LOG career_shop: left the shop' \
  "waitfng MainMenu_Sub.fng'2,000 satisfied" "waitfng MC_Main.fng'Save_complete. satisfied" \
  'LOG career_shop: saved') || fail "missing, or out of order: $miss"
# each category's RETURN left the category for a screen of its own (or a dialog on it): the waits after them
n=$(grep -c 'waitfng -CustomizeCategory.fng|Dialog.fng satisfied' "$LOG")
[ "$n" -eq 19 ] || fail "want 19 category screens opened, got $n"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "the shop's 19 categories opened and closed; the Reaper spoiler bought (funds 3,000 -> 2,000); the shop left;" \
  "the safehouse showed Cash 2,000 and saved; the game left by WM_CLOSE"
