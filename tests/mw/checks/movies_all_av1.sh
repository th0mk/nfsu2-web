#!/bin/bash
# Suite check for "movies_all_av1" (tests/mw/scripts/movies.txt run with --name movies_all_av1, TRACKER M7-10), from a
# server whose game folder holds the AV1 copies (MOVIES/AV1) and a browser that decodes AV1: movies.sh's checks (each
# of the 32 movies up, played to its end, up again and skipped by input; each full play as long as its file), and
# every play through the AV1 hook: 64 "[movie] AV1 1024x512" lines after the first movie's start (32 full plays and 32
# skipped; the boot movies before it, cut short by the presses to the main menu, through the hook too), each with a
# picture for every frame decoded, no catch-up, no VP6 play (NFSU2_MOVIE_LOG=1 logs one).  Each movie's picture is
# decoded as the game decodes its frame's chunk, so a play's pictures keep the game's own timing against its sound:
# with no frame without a picture and no catch-up the picture shown is the one of the frame the player is at.
LOG="$1"
fail(){ echo "$1"; exit 1; }
out=$("$(dirname "$0")/movies.sh" "$LOG") || fail "movies.sh: $(echo "$out" | tail -1)"
grep -qE '^\[movie\] VP6' "$LOG" && fail "a movie played through the VP6 decoder: $(grep -m1 -E '^\[movie\] VP6' "$LOG")"
# each AV1 play after the first movie's start, named by the movie the script played last ("<name>: <line>")
A=$(awk '/LOG movie [a-z0-9_]+: start$/ { on = 1; m = $0; sub(/.*LOG movie /, "", m); sub(/: start$/, "", m) }
  on && /^\[movie\] AV1 [0-9]+x[0-9]+: / { print m ": " $0 }' "$LOG")
n=$(echo "$A" | grep -c .)
[ "$n" = 64 ] || fail "$n AV1 plays after the first movie's start, 64 wanted (32 movies, played to the end and skipped)"
b=$(awk '/LOG movie [a-z0-9_]+: start$/ { exit } /^\[movie\] AV1 1024x512: / { n++ } END { print n + 0 }' "$LOG")
[ "$b" -ge 1 ] || fail "no boot movie through the AV1 hook"
echo "$A" | grep -qvE ': \[movie\] AV1 1024x512: ' && fail "an AV1 play not at 1024x512"
m=$(echo "$A" | grep -cvE ' 0 without a picture, ')
[ "$m" = 0 ] || fail "$m plays had frames without a picture: $(echo "$A" | grep -m1 -vE ' 0 without a picture, ')"
k=$(echo "$A" | grep -cvE ' 0 catch-ups$')
[ "$k" = 0 ] || fail "$k plays caught up: $(echo "$A" | grep -m1 -vE ' 0 catch-ups$')"
fr=$(echo "$A" | sed -E 's/.*: ([0-9]+) frames, .*/\1/' | awk '{ s += $1 } END { print s }')
worst=$(echo "$A" | sed -E 's/.*\(max ([0-9.]+)\).*/\1/' | sort -g | tail -1)
echo "$out" | sed -n '1,32p'
echo "64 plays through the AV1 hook at 1024x512 ($fr frames; $b boot movies before them), every frame with its picture, no catch-up, the slowest frame $worst ms; none through VP6"
