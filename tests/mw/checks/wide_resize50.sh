#!/bin/bash
# Suite check for "wide_resize50" (tests/mw/scripts/wide_resize50.txt, TRACKER M7-04, PLAN G-08): the module under
# test is named (L-020); the page took 50 sizes (the play page's "[play] canvas resized" lines, &resizeon) and the
# device was made again for each (com.c's "device resolution" lines from the game's Reset); after the 50th change
# (the first size again) the texture ids live and made, the kernel objects, file handles and threads ([status]
# tex=live/made obj= fh= thr=) are where they were after the first round of five, back at that size; the picture was
# written; no stall; the game left by WM_CLOSE.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^([0-9.]+ )?module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q 'LOG wide_resize50: done' "$LOG" || fail "the script did not end"
n=$(grep -c '\[play\] canvas resized to .* (resize ' "$LOG")
[ "$n" -ge 50 ] || fail "the page changed the canvas $n times, not 50 (&resizeon on the play page)"
d=$(grep -cE 'com: device resolution [0-9]+x[0-9]+ -> [0-9]+x[0-9]+' "$LOG")
[ "$d" -ge 50 ] || fail "the device was made $d times after the first, not 50"
mapfile -t R < <(grep -E '\[status\] resized_[0-9]+ ' "$LOG")
[ "${#R[@]}" -ge 50 ] || fail "${#R[@]} [status] lines after a change, not 50"
f(){ echo "$1" | grep -oE " $2=[0-9/]+" | head -1 | sed "s/ $2=//"; }
a="${R[4]}" b="${R[${#R[@]}-1]}"
for k in tex obj fh thr; do
  [ "$(f "$a" $k)" = "$(f "$b" $k)" ] || fail "$k after the 5th change $(f "$a" $k), after the 50th $(f "$b" $k)"
done
grep -qE "\[gpu\] dump (.*_)?nfsu2_g[0-9]+_menu_after\.png " "$LOG" || fail "no picture of menu_after"
grep -qF '[watchdog] no Present' "$LOG" && fail "a stall: $(grep -m1 -F '[watchdog] no Present' "$LOG")"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
grep -qE 'quit at guest frame|forcing the exit' "$LOG" && fail "the run was ended for the game, not by it"
echo "50 changes, $d devices; tex $(f "$b" tex) obj $(f "$b" obj) fh $(f "$b" fh) thr $(f "$b" thr) as after the first round; the game left by WM_CLOSE"
