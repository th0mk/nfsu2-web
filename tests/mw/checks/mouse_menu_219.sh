#!/bin/bash
# Suite check for "mouse_menu_219" (tests/mw/scripts/mouse_menu_219.txt, TRACKER M7-03): mouse_menu.txt's walk at
# 21:9, the same screens, log lines and [expect] count, so mouse_menu.sh's check; the device must be 21:9.
LOG="$1"
dev=$(grep -m1 -oE 'com: device resolution [0-9]+x[0-9]+ -> [0-9]+x[0-9]+' "$LOG") || { echo "no device resolution line"; exit 1; }
wh=${dev##*-> }; w=${wh%x*}; h=${wh#*x}
[ $((w * 9)) -eq $((h * 21)) ] || { echo "the device is ${wh}, not 21:9 (NFSRES=2520x1080)"; exit 1; }
exec "$(dirname "$0")/mouse_menu.sh" "$LOG"
