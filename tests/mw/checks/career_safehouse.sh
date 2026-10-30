#!/bin/bash
# Suite check for "career_safehouse" (tests/mw/scripts/career_safehouse.txt, TRACKER M4-10), a start seeded with the save
# career_start.txt left: the module under test is named (L-020); every wait in time and every expect passed; Resume
# Career put the car in free roam at the prologue's bin (16), whose pause menu had its three items and its options
# menu; the drive into race_bin_16's drive_to_safe_house trigger loaded the safehouse; Sonny's bio came with the bin
# at 15; the four Blacklist screens opened and closed; the safehouse menu's four items were used (the Blacklist, Car
# Select, a save that completed, Resume Free Roam); the bin-15 pause menu's items and the world map opened and
# closed; Jump to Safe House came back to the safehouse menu; all in that order; the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
# the fixed strings given must occur in the log in this order, each on a later line; prints the first one missing
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q '^\[expect\] FAIL' "$LOG" && fail "an expect failed: $(grep -m1 '^\[expect\] FAIL' "$LOG")"
[ "$(grep -c '^\[expect\] PASS' "$LOG")" -eq 2 ] || fail "want 2 passed expects (bin 16, then 15), got $(grep -c '^\[expect\] PASS' "$LOG")"
miss=$(inorder 'LOG career_safehouse: free roam at bin 16' \
  'waitfng Pause_Main.fng:690e9b7c@4b4e388b satisfied' 'waitfng Pause_Main.fng:1d7bb6c9 satisfied' \
  'LOG career_safehouse: pause menu (bin 16) done' 'LOG career_safehouse: drove to the safehouse' \
  'waitfng SafeHouseRivalBio.fng:cace5999 satisfied' 'LOG career_safehouse: Blacklist intro' \
  'waitfng SafeHouseRaceSheet.fng@09c9a0f9 satisfied' 'waitfng SafeHouseMilestones.fng@09c9a0f9 satisfied' \
  'waitfng SafeHouseBounty.fng@09c9a0f9 satisfied' 'LOG career_safehouse: Blacklist screens' \
  'LOG career_safehouse: safehouse menu' 'waitfng Car_Select.fng:ee053562 satisfied' \
  "waitfng MC_Main.fng'Save_complete. satisfied" 'LOG career_safehouse: saved' \
  'waitfng Dialog.fng@b8a7c6cc satisfied' 'LOG career_safehouse: free roam at bin 15' \
  'waitfng Pause_Main.fng:3c14c420@4b4e388b satisfied' 'waitfng Pause_Main.fng:6a3672a2@4b4e388c satisfied' \
  'LOG career_safehouse: pause menu (bin 15) done' 'LOG career_safehouse: world map' \
  'waitfng -WorldMapMain.fng satisfied' 'waitfng InGameDialog.fng@b8a7c6cc satisfied' \
  'LOG career_safehouse: jumped') || fail "missing, or out of order: $miss"
# the free roam after the safehouse begins at the safehouse gate's respawn point (-2508.82, 1762.76)
r=$(awk '/LOG career_safehouse: free roam at bin 15$/ { p = 1 } p && /^\[mwrace\] f=[0-9]+ where -?[0-9]/ {
  printf "%.1f", sqrt(($4 + 2508.82) ^ 2 + ($6 - 1762.76) ^ 2); exit }' "$LOG")
[ -n "$r" ] || fail "no position logged after leaving the safehouse"
awk -v d="$r" 'BEGIN { exit !(d <= 20) }' || fail "the car left the safehouse $r m from its respawn point (want 20 m or less)"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "free roam at bin 16, its pause menu; the drive into the safehouse trigger; the Blacklist intro at bin 15 and its four screens; the safehouse menu's four items, a completed save; the bin-15 pause menu, the world map, Jump to Safe House; the game left by WM_CLOSE"
