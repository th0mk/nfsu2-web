#!/bin/bash
# Suite check for "boot_checks" (tests/mw/scripts/boot_checks.txt): boot_menu.sh's checks, then
#   - the 100 Hz timer: [0x9BB130] rose by 100 +/- 10 per second of the traced span (the [trace] lines' t=, ms);
#   - the effects: 32 "[expect] PASS" lines (the pool and the 31 effect objects; a FAIL fails G-01 by itself);
#   - server.dll mapped at 0x00d00000 with its pin, DllMain attach and detach both 1.
# The first start or not is the run's to show (the peeks of [0x901820] and the [reg] lines): it is not checked here.
LOG="$1"
fail(){ echo "$1"; exit 1; }
"$(dirname "$0")/boot_menu.sh" "$LOG" >/dev/null || fail "boot_menu: $("$(dirname "$0")/boot_menu.sh" "$LOG")"
rate=$(grep -E '^\[trace\] f=[0-9]+ t=[0-9]+ .* tick=' "$LOG" | sed -E 's/.* t=([0-9]+) .* tick=(-?[0-9]+).*/\1 \2/' \
  | awk 'NR == 1 { t0 = $1; v0 = $2 } { t1 = $1; v1 = $2 } END { if (t1 - t0 >= 3000) printf "%.1f", (v1 - v0) * 1000 / (t1 - t0) }')
[ -n "$rate" ] || fail "no 3 s of [trace] lines with the timer count"
awk -v r="$rate" 'BEGIN { exit !(r >= 90 && r <= 110) }' || fail "the timer count rose $rate per second, not about 100"
n=$(grep -c '^\[expect\] PASS' "$LOG")
[ "$n" -eq 32 ] || fail "$n of 32 effect checks passed"
grep -qE '^\[module\] server\.dll mapped at 0x00d00000-' "$LOG" || fail "server.dll was not mapped at 0x00d00000"
grep -qF '[module] server.dll: DllMain(PROCESS_ATTACH) -> 1' "$LOG" || fail "server.dll's DllMain did not attach"
grep -qE '^\[module\] server\.dll: DllMain\(PROCESS_DETACH\) -> 1' "$LOG" || fail "server.dll's DllMain did not detach"
echo "boot_menu's checks; the timer count rose $rate per second; the pool and the 31 effects created; server.dll at 0x00d00000"
