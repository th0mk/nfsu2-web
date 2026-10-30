#!/bin/bash
# Suite check for "career_safehouse_load" (tests/mw/scripts/career_safehouse_load.txt, TRACKER M4-10), a start seeded
# with the save career_safehouse.txt left in the safehouse: the module under test is named (L-020); every wait in time
# and both expects passed; Resume Career brought the safehouse menu with no world between (no Loading_Tips.fng, the
# front end's flow), bin 15 and Cash 3,000; Resume Free Roam put the car within 20 m of the safehouse gate's respawn
# point; the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q '^\[expect\] FAIL' "$LOG" && fail "an expect failed: $(grep -m1 '^\[expect\] FAIL' "$LOG")"
[ "$(grep -c '^\[expect\] PASS' "$LOG")" -eq 2 ] || fail "want 2 passed expects, got $(grep -c '^\[expect\] PASS' "$LOG")"
D='"C:\NFSMW\NFS Most Wanted'
miss=$(inorder "[file] CreateFileA($D/ALPHA/ALPHA\") -> " 'LOG career_safehouse_load: Resume Career' \
  'waitfng MainMenu_Sub.fng:e596c4a3@4b4e388a satisfied' "waitfng MainMenu_Sub.fng'3,000 satisfied" \
  'LOG career_safehouse_load: safehouse menu' 'LOG career_safehouse_load: free roam') || fail "missing, or out of order: $miss"
tips=$(awk '/LOG career_safehouse_load: Resume Career$/ { p = 1 } /LOG career_safehouse_load: safehouse menu$/ { p = 0 }
  p && /screen Loading_Tips.fng/ { print; exit }' "$LOG")
[ -z "$tips" ] || fail "a world loaded between Resume Career and the safehouse menu: $tips"
r=$(awk '/LOG career_safehouse_load: free roam$/ { p = 1 } p && /^\[mwrace\] f=[0-9]+ where -?[0-9]/ {
  printf "%.1f", sqrt(($4 + 2508.82) ^ 2 + ($6 - 1762.76) ^ 2); exit }' "$LOG")
[ -n "$r" ] || fail "no position logged in free roam"
awk -v d="$r" 'BEGIN { exit !(d <= 20) }' || fail "the car is $r m from the safehouse's respawn point (want 20 m or less)"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "after a restart: ALPHA loaded, Resume Career opened the safehouse menu (bin 15, Cash 3,000), Resume Free Roam" \
  "put the car $r m from the safehouse's respawn point; the game left by WM_CLOSE"
