#!/bin/bash
# Suite check for "mouse_menu_169" (tests/mw/scripts/mouse_menu_169.txt, TRACKER M7-03): mouse_menu.txt's walk at
# 16:9, the same screens, log lines and [expect] count, so mouse_menu.sh's check; the device must be 16:9.
LOG="$1"
dev=$(grep -m1 -oE 'com: device resolution [0-9]+x[0-9]+ -> [0-9]+x[0-9]+' "$LOG") || { echo "no device resolution line"; exit 1; }
wh=${dev##*-> }; w=${wh%x*}; h=${wh#*x}
[ $((w * 9)) -eq $((h * 16)) ] || { echo "the device is ${wh}, not 16:9 (NFSRES=1280x720 or 1920x1080)"; exit 1; }
exec "$(dirname "$0")/mouse_menu.sh" "$LOG"
