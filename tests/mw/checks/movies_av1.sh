#!/bin/bash
# Suite check for "movies_av1" (tests/mw/scripts/movies_av1.txt, TRACKER M7-10), from a server whose game folder holds
# the AV1 copies (MOVIES/AV1) and a browser that decodes AV1: the module under test is named (L-020); the boot movies
# and blacklist_15 each played through the AV1 hook (runtime/mw/movie_mw.c, runtime/movie_av1.c): one "[movie] AV1
# 1024x512" line a play, at least 4, each with a picture for every frame decoded; no VP6 play (NFSU2_MOVIE_LOG=1 logs
# one); no wait ran out; the game left by WM_CLOSE.  With NFSMW_DATA=<the game folder> each play's frames are set
# against its file's MVhd count (+16): the decodes of a play to the end are all of its frames but the last one or two,
# and a catch-up (the player 16 frames behind) would show.  Prints each play.
LOG="$1"
fail(){ echo "$1"; exit 1; }
# the movies asked in another language or region that played the English one in their place (game_mw.c file_alt)
alts(){
  local a; a=$(grep -oE '\[file\] "[^"]*MOVIES[\\/][^"]*" is not there: "[^"]*" in its place' "$LOG" \
    | sed -E 's/.*MOVIES[\\/]([^"]*)" is not there: ".*MOVIES[\\/]([^"]*)" in its place/\1 -> \2/' | sort -u)
  [ -n "$a" ] && echo "  $(echo "$a" | grep -c .) movie names in another language's place: $(echo "$a" | head -3 | tr '\n' ',' | sed 's/,$//; s/,/, /g'), ..."
  return 0
}
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -qE 'LOG movies_av1: blacklist_15 end$' "$LOG" || fail "blacklist_15 did not reach its end"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE '^\[movie\] VP6' "$LOG" && fail "a movie played through the VP6 decoder: $(grep -m1 -E '^\[movie\] VP6' "$LOG")"
grep -q 'this page has no AV1 decoder' "$LOG" && fail "the page had no AV1 decoder"
A=$(grep -E '^\[movie\] AV1 [0-9]+x[0-9]+: ' "$LOG")
n=$(echo "$A" | grep -c .)
[ "$n" -ge 4 ] || fail "$n AV1 plays, 4 wanted (the EA logo, the PSA, the attract movie, blacklist_15)"
echo "$A" | grep -qvE '^\[movie\] AV1 1024x512: ' && fail "an AV1 play not at 1024x512: $(echo "$A" | grep -m1 -vE '^\[movie\] AV1 1024x512: ')"
bad=0 i=0
for name in ealogo psa attract_movie blacklist_15; do
  i=$((i + 1))
  l=$(echo "$A" | sed -n "${i}p")
  frames=$(echo "$l" | sed -E 's/.*: ([0-9]+) frames, .*/\1/'); missed=$(echo "$l" | sed -E 's/.* ([0-9]+) without a picture.*/\1/')
  catch=$(echo "$l" | sed -E 's/.* ([0-9]+) catch-ups$/\1/'); ms=$(echo "$l" | sed -E 's/.*decode ([0-9.]+) ms a frame \(max ([0-9.]+)\).*/\1 (max \2)/')
  want="" note=""
  f="${NFSMW_DATA:+$NFSMW_DATA/MOVIES/${name}_english_ntsc.vp6}"
  if [ -n "$f" ] && [ -f "$f" ]; then
    want=$(od -An -tu4 -j16 -N4 "$f" | tr -d ' ')
    [ "$frames" -ge $((want - 2)) ] && [ "$frames" -le "$want" ] || { note=" NOT ALL FRAMES"; bad=1; }
  fi
  [ "$missed" = 0 ] || { note="$note $missed WITHOUT A PICTURE"; bad=1; }
  printf '  %-14s AV1 %5s frames%s, %s without a picture, decode %s ms a frame, %s catch-ups%s\n' "$name" "$frames" \
    "${want:+ of $want}" "$missed" "$ms" "$catch" "$note"
done
[ "$bad" = 0 ] || fail "a play missed pictures or frames (above)"
alts
echo "$n plays through the AV1 hook at 1024x512, every frame with its picture; none through VP6"
