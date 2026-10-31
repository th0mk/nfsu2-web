#!/bin/bash
# Suite check for "pad_dpad" (tests/mw/scripts/pad_dpad.txt, TRACKER M6-37): the module under test is named (L-020);
# every wait in time; free roam reached; the D-pad's right brought up SMS_MailBoxes.fng and B took it away; its left
# brought up WorldMapMain.fng and B took it away, in that order; the game left by WM_CLOSE.  Its down is only logged.
LOG="$1"
fail(){ echo "$1"; exit 1; }
inorder(){ awk 'BEGIN { n = ARGC - 2; for (i = 1; i <= n; i++) { p[i] = ARGV[i]; delete ARGV[i] } k = 1 }
                k <= n && index($0, p[k]) { k++ }
                END { if (k <= n) { print p[k]; exit 1 } }' "$@" "$LOG"; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -qE '^\[autotest\] f=[0-9]+ .* TIMEOUT' "$LOG" && fail "a wait of the script ran out: $(grep -m1 -E 'TIMEOUT' "$LOG")"
miss=$(inorder 'LOG pad_dpad: free roam' 'screen SMS_MailBoxes.fng' 'LOG pad_dpad: messages up' \
  'waitfng -SMS_MailBoxes.fng satisfied' 'LOG pad_dpad: messages gone' 'LOG pad_dpad: B after down' \
  'screen WorldMapMain.fng' 'LOG pad_dpad: map up' 'waitfng -WorldMapMain.fng satisfied' 'LOG pad_dpad: done') \
  || fail "missing, or out of order: $miss"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
echo "the D-pad's right brought up the SMS messages and its left the World Map, each taken away by B"
