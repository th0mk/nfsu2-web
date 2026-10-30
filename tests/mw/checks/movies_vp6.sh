#!/bin/bash
# Suite check for "movies_vp6" (tests/mw/scripts/movies_av1.txt run with --name movies_vp6 and NFSU2_MOVIE_LOG=1,
# TRACKER M7-10): where the page plays no AV1 (NFSU2_MOVIE_CODEC=vp6, a browser without AV1, a folder without the
# copies) the same movies play through the game's own VP6 decoder: the module under test is named (L-020); the boot
# movies and blacklist_15 reached their ends, each freed decoder logged "[movie] VP6: N catch-ups" (at least 4) and the
# planes' layout of a 1024x512 VP6 picture was logged; no AV1 play; no wait ran out; the game left by WM_CLOSE.
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
grep -qE '^\[movie\] AV1' "$LOG" && fail "a movie played from an AV1 copy: $(grep -m1 -E '^\[movie\] AV1' "$LOG")"
grep -qE '^\[movie\] VP6 1024x512 strides 1120/560, planes at \+53808/\+[0-9]+/\+[0-9]+ of the buffer$' "$LOG" \
  || fail "no 1024x512 VP6 picture with a 48-pixel border logged: $(grep -m1 -E '^\[movie\] VP6 [0-9]' "$LOG")"
n=$(grep -cE '^\[movie\] VP6: [0-9]+ catch-ups$' "$LOG")
[ "$n" -ge 4 ] || fail "$n VP6 decoders freed, 4 wanted"
alts
echo "$n plays through the game's VP6 decoder ($(grep -m1 -E '^\[movie\] VP6 1024x512' "$LOG" | cut -c9-)); none from an AV1 copy"
