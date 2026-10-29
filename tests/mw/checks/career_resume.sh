#!/bin/bash
# Suite check for "career_resume" (tests/mw/scripts/career_resume.txt, TRACKER M4-09), a start seeded with the save
# career_start.txt left: the module under test is named (L-020); every wait in time; the start read the seeded
# ALPHA, the main menu came, Career showed the career menu of an alias with a career (Resume Career), and
# Resume Career put the car in free roam: GameFlow 6, a career with no race (0 racers, before the drive and after
# it), no prologue race loaded (no NIS scene bundle opened after Resume Career: every prologue race opens one while
# its world loads) and none going active in the run, the car within 50 m of where the career's free roam begins
# (-2148.45 153.03 981.90 in the runs that made the seed), then moved at least 5 m by the gas held 300 frames; all
# in that order; the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
# the fixed strings given must occur in the log in this order, each on a later line; prints the first one missing
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
D='"C:\NFSMW\NFS Most Wanted'
miss=$(inorder "[file] CreateFileA($D/ALPHA/ALPHA\") -> " 'LOG career_resume: ALPHA loaded' \
  'waitfng MainMenu_Sub.fng:e072db21@4b4e388a satisfied' 'LOG career_resume: Resume Career' \
  'autopilot races: the AI takes each race at its GO' 'LOG career_resume: free roam' \
  'racestatus flow 6 racers 0 active 0 won 0 player -1: none; mode 2' 'LOG career_resume: drove' \
  'racestatus flow 6 racers 0 active 0 won 0 player -1: none; mode 2') || fail "missing, or out of order: $miss"
nis=$(awk '/LOG career_resume: Resume Career$/ { p = 1 } p && index($0, "/NIS\\Scene_") { print; exit }' "$LOG")
[ -z "$nis" ] || fail "a prologue scene loaded after Resume Career:$nis"
grep -qE '^\[mwrace\] f=[0-9]+ race: active 1' "$LOG" && fail "a race started: $(grep -m1 -E '^\[mwrace\] f=[0-9]+ race: active 1' "$LOG")"
# the two positions logged after "free roam": the first near the free roam's start, the second 5 m or more from it
r=$(awk 'BEGIN { n = 0 } /LOG career_resume: free roam$/ { p = 1 } p && /^\[mwrace\] f=[0-9]+ where -?[0-9]/ { x[n] = $4; z[n] = $6; n++ }
  END { if (n < 2) { print "-1 -1"; exit }
        printf "%.1f %.1f", sqrt((x[0] + 2148.45) ^ 2 + (z[0] - 981.90) ^ 2), sqrt((x[1] - x[0]) ^ 2 + (z[1] - z[0]) ^ 2) }' "$LOG")
s=${r% *} d=${r#* }
awk -v s="$s" 'BEGIN { exit !(s >= 0 && s <= 50) }' || fail "the car is $s m from the free roam's start (want 50 m or less)"
awk -v d="$d" 'BEGIN { exit !(d >= 5) }' || fail "the gas moved the car $d m (want 5 m or more)"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "after a restart: ALPHA loaded, Resume Career put the car in free roam ($s m from its start, no race), the gas" \
  "moved it $d m; the game left by WM_CLOSE"
