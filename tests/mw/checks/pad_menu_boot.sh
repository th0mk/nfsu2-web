#!/bin/bash
# Suite check for "pad_menu_boot" (tests/mw/scripts/pad_menu_boot.txt): tests/mw/checks/pad_menu.sh with the pad
# there at the start.
exec bash "$(dirname "$0")/pad_menu.sh" "$1" boot
