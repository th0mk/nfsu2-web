#!/bin/bash
# Suite check for "datafault" (tests/mw/scripts/boot_menu.txt under NFSU2_DATA_FAULT, TRACKER M3-24, L-060): the
# boot to the main menu passes tests/mw/checks/boot_menu.sh while the page's data reads (web/src/host/io-worker.ts)
# meet every fault the scenario asks for, each seen in its [data] line: a lost connection asked again, a request left
# unanswered until its stall timer, a block whose compressed address never answers read as a byte range, and the
# cache on this PC full (the read goes on from the server).
LOG="$1"
fail(){ echo "$1"; exit 1; }
out=$(bash "$(dirname "$0")/boot_menu.sh" "$LOG") || fail "$out"
grep -qE '\[data\] .*: lost connection \(NFSU2_DATA_FAULT\).*; again in ' "$LOG" || fail "no lost connection was asked again"
grep -qE '\[data\] .*: no answer for [0-9.]+ s.*; again in ' "$LOG" || fail "no unanswered request ran into its stall timer"
grep -qE '\[data\] .*: in after [0-9.]+ s, [0-9]+ tries \(as a byte range\)' "$LOG" || fail "no block came as a byte range after its compressed address failed"
grep -qF '[data] no room on this PC to keep more of the game data' "$LOG" || fail "the cache never filled up"
echo "$out; under lost, unanswered and denied requests and a full cache"
