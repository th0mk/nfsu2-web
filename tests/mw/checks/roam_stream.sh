#!/bin/bash
# Suite check for "roam_stream" (tests/mw/scripts/roam_stream.txt, TRACKER M4-23): the module under test is named
# (L-020); every wait in time; the world map opened and closed before the drive; every pass of the drive
# (six legs of STREAM_N when the environment names it) logged the car's place; no "[data]" line of a read that waited
# on the server (io-worker.ts: "waiting for the server", "in after", a try again, a failed read) and no loading screen (a package named Loading*, NFSU2_FNG_LOG) from the drive's start to
# its end; the car moved 50 m or more (and less than 900: a leg's teleport is no drive) in at least three passes of
# four; the game left by WM_CLOSE.  Prints the distance driven, the mean and top speed seen and the file handles open
# (fh, the [status] lines: L-061).
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
for m in "world map" drive drove done; do grep -q "LOG roam_stream: $m\$" "$LOG" || fail "no '$m'"; done
W='^\[data\] .*(waiting for the server|: in after|again in [0-9.]+ s|failed|did not read)'
grep -qE "$W" "$LOG" && fail "a read waited on the server: $(grep -m1 -E "$W" "$LOG")"
ahead=$(grep -m1 -E '^\[data\] TRACKS/STREAM[^:]*: all of it on this device now' "$LOG" | sed 's/^\[data\] //')
out=$(awk '
  /LOG roam_stream: drive$/ { on = 1; next }
  /LOG roam_stream: drove$/ { on = 0 }
  on && /^\[fng\] .*screen Loading/ { load++; if (!lw) lw = $0 }
  on && /^\[mwrace\] f=[0-9]+ where / { for (i = 1; i <= NF; i++) if ($i == "where") break
    x = $(i + 1); z = $(i + 3); s = $(i + 5) + 0; n++; ss += s; if (s > top) top = s
    if (n > 1) { dd = sqrt((x - px) ^ 2 + (z - pz) ^ 2); if (dd < 900) { d += dd; if (dd >= 50) mv++ } }
    px = x; pz = z }
  on && /^\[status\] stream(_[0-9]+)? / { if (match($0, /fh=[0-9]+/)) { h = substr($0, RSTART + 3, RLENGTH - 3) + 0; if (!hn++ || h < hmin) hmin = h; if (h > hmax) hmax = h } }
  END { printf "passes %d moving %d distance %.1f km mean %.0f km/h top %.0f km/h fh %d..%d loads %d %s\n", n, mv, d / 1000, n ? ss / n : 0, top, hmin, hmax, load, lw }' "$LOG")
n=$(echo "$out" | awk '{ print $2 }'); mv=$(echo "$out" | awk '{ print $4 }')
[ -n "$STREAM_N" ] && [ "$n" != $((6 * STREAM_N)) ] && fail "$n of $((6 * STREAM_N)) passes: $out"
[ "$n" -ge 2 ] || fail "only $n passes: $out"
[ $((mv * 4)) -ge $((n * 3)) ] || fail "the car moved in only $mv of its $n passes: $out"
echo "$out" | grep -q ' loads 0 ' || fail "a loading screen in the drive: $out"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "the world streamed with no wait on the server and no loading screen: $out; ${ahead:-the stream file not read ahead}"
