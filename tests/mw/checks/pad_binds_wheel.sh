#!/bin/bash
# Suite check for "pad_binds_wheel" (tests/mw/scripts/pad_binds_wheel.txt, TRACKER M6-37, M6-38): pad_binds.sh with
# the wheel's layout.
KIND=wheel exec bash "$(dirname "$0")/pad_binds.sh" "$@"
