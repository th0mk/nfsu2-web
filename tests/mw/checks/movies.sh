#!/bin/bash
# Suite check for "movies" (tests/mw/scripts/movies.txt, TRACKER M4-41): the module under test is named (L-020);
# each of the 32 movies came up, played to its end and came up again, and the press 6 s into that second play ended
# it within a second for every movie longer than 7 s (the shorter ones end by themselves first); no wait ran out,
# and the game left by WM_CLOSE.  Each full play lasted its length (less 250 ms and 0.1 %): with NFSMW_DATA=<the game
# folder>, the frame count in its MVhd header (+16) at 982047/32767 fps (GAME 8.1), else 2.5 s.  Prints each movie's
# times.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
for m in start end press skipped; do
  n=$(grep -cE "LOG movie [a-z0-9_]+: $m\$" "$LOG")
  [ "$n" = 32 ] || fail "$n of 32 movies reached '$m'"
done
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
# name, the full play's ms up, the skipped play's ms from the press (the movieend lines: "movie gone after N frames,
# T ms; up U frames, P ms", T from the press, P from when movieup saw it)
T=$(awk '/LOG movie [a-z0-9_]+: start$/ { match($0, /LOG movie [a-z0-9_]+/); cur = substr($0, RSTART + 10, RLENGTH - 10); ph = 1; next }
  /LOG movie [a-z0-9_]+: press$/ { ph = 2; next }
  /^\[mwtest\] f=[0-9]+ movie gone after/ { if (ph == 1) play[cur] = $(NF - 1); else if (ph == 2) { t = $0; sub(/ ms;.*/, "", t); sub(/.* /, "", t); skip[cur] = t; order[n++] = cur } ph = 0 }
  END { for (i = 0; i < n; i++) print order[i], play[order[i]] + 0, skip[order[i]] + 0 }' "$LOG")
[ "$(echo "$T" | grep -c .)" = 32 ] || fail "not every movie has a full and a skipped play: $(echo "$T" | grep -c .)"
bad=0
while read -r name play skip; do
  want=2500
  f="${NFSMW_DATA:+$NFSMW_DATA/MOVIES/${name}_english_ntsc.vp6}"
  if [ -n "$f" ] && [ -f "$f" ]; then
    frames=$(od -An -tu4 -j16 -N4 "$f" | tr -d ' ')
    want=$(( frames * 32767 * 1000 / 982047 ))
  fi
  # the screen's time up is measured from frame to frame (a frame or two at either end) on the page's clock
  min=$(( want - 250 - want / 1000 ))
  note=""
  [ "$play" -ge "$min" ] || { note=" SHORT (want >= $min ms)"; bad=1; }
  if [ "$play" -gt 7000 ] && [ "$skip" -gt 1000 ]; then note="$note NOT SKIPPED"; bad=1; fi
  printf '  %-24s played %6d ms (length %6d ms), gone %4d ms after the press%s\n' "$name" "$play" "$want" "$skip" "$note"
done <<< "$T"
[ "$bad" = 0 ] || fail "a movie played short or was not skipped (above)"
echo "32 movies played to their end and skipped by input"
