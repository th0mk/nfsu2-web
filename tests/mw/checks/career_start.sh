#!/bin/bash
# Suite check for "career_start" (tests/mw/scripts/career_start.txt, TRACKER M4-09): the module under test is named
# (L-020); every wait in time; no racewin; ALPHA made and saved, then the career menu of a new alias (Start Career)
# and the prologue: the intro NIS opened, five races taken by the AI at their GO (more when a lost race was raced
# again: each loss's results and the career menu's Resume Career pressed by runtime/mw/career_mw.c, one GO more),
# races 2, 3 and 4 won over the line (the winner's screen, then the race status: the player racer 1, slot #0
# finished, the race won), "six days later" between races 3 and 4, the race over and no longer racing after race 5
# (the world unloads), the car lot, the car bought; then free roam (GameFlow 6, a career with no race: 0 racers),
# ALPHA written by the free roam's autosave after the purchase, the pause menu gone after Resume Free Roam, and the
# car moved at least 5 m by the gas held 300 frames; all in that order; the game left by WM_CLOSE.  Prints the frames from Start Career to free roam, the races
# raced again, and the distance driven.
LOG="$1"
fail(){ echo "$1"; exit 1; }
# the fixed strings given must occur in the log in this order, each on a later line; prints the first one missing
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -qE '^\[mwrace\] f=[0-9]+ racewin' "$LOG" && fail "racewin ended a race: $(grep -m1 -E '^\[mwrace\] f=[0-9]+ racewin' "$LOG")"
D='"C:\NFSMW\NFS Most Wanted'
W="[write] CreateFileA(CREATE_ALWAYS, 0x40000000) $D/ALPHA/ALPHA\""
WON='won 1 player 1: #0 100.0% finished'
miss=$(inorder "$W" 'LOG career_start: ALPHA saved' 'waitfng MainMenu_Sub.fng:6005281e@4b4e388a satisfied' \
  'Scene_IntroNisDD_BundleB.bun' 'LOG career_start: prologue' 'autopilot races: the AI takes each race at its GO' \
  "autopilot on: the race's GO" "autopilot on: the race's GO" 'LOG career_start: race 2 won' "$WON" \
  "autopilot on: the race's GO" 'LOG career_start: race 3 won' "$WON" 'screen SixDaysLater.fng' \
  "autopilot on: the race's GO" 'LOG career_start: race 4 won' "$WON" "autopilot on: the race's GO" \
  'race: active 0 go 0 #0 finished 0 won 0 (flow ' 'waitfng Car_Select.fng:d18d600a satisfied' \
  'LOG career_start: car lot' 'tapuntil Dialog.fng@b8a7c6cc satisfied' "$W" 'waitfng SMS_Message.fng:8cd6bd6d satisfied' \
  'LOG career_start: free roam' 'racestatus flow 6 racers 0 active 0 won 0 player -1: none; mode 2' \
  'waitfng SMS_MailBoxes.fng@cf31e520 satisfied' 'tapuntil Pause_Main.fng:01bd185c@4b4e388a satisfied' \
  'waitfng -Pause_Main.fng satisfied' 'LOG career_start: drove') || fail "missing, or out of order: $miss"
# five races started in the prologue, and one more for each lost race raced again; three winner's screens
lost=$(grep -cE '^\[mwrace\] f=[0-9]+ autopilot: the race was lost: Enter on its results$' "$LOG")
n=$(awk '/LOG career_start: prologue$/ { p = 1 } /LOG career_start: car lot$/ { p = 0 }
  p && /autopilot on: the race.s GO/ { n++ } END { print n + 0 }' "$LOG")
[ "$n" = $((5 + lost)) ] || fail "$n races reached GO in the prologue, not 5 and $lost raced again"
n=$(grep -c 'waitfng InGamePhotoMaster.fng:5dfb6667 satisfied' "$LOG")
[ "$n" = 3 ] || fail "$n winner's screens, not 3"
# the drive: the two positions logged after "free roam" (before and after the gas), apart in the ground plane
d=$(awk 'BEGIN { n = 0 } /LOG career_start: free roam$/ { p = 1 } p && /^\[mwrace\] f=[0-9]+ where -?[0-9]/ { x[n] = $4; z[n] = $6; n++ }
  END { if (n < 2) print -1; else printf "%.1f", sqrt((x[1] - x[0]) ^ 2 + (z[1] - z[0]) ^ 2) }' "$LOG")
awk -v d="$d" 'BEGIN { exit !(d >= 5) }' || fail "the gas moved the car $d m (want 5 m or more)"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
f=$(awk '/waitfng MainMenu_Sub.fng:6005281e@4b4e388a satisfied/ { match($0, /f=[0-9]+/); a = substr($0, RSTART + 2, RLENGTH - 2) }
  /LOG career_start: free roam$/ { match($0, /f=[0-9]+/); print substr($0, RSTART + 2, RLENGTH - 2) - a; exit }' "$LOG")
echo "ALPHA made; the prologue played (5 races, 3 won by the AI over the line, $lost lost and raced again; race 5" \
  "ended by the game), the car bought, free roam saved ($f frames after Start Career), the car driven $d m; the game" \
  "left by WM_CLOSE"
