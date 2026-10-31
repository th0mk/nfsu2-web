#!/bin/bash
# Suite check for "music_modes" (tests/mw/scripts/music_modes.txt), run with NFSMW_EALAYER3_LOG=1 and a module and data
# folder with the EALayer3 music pack: the front end, free roam and the pursuit each got music through the runtime's
# decoder (voices created after each of the script's marks), none of its voices failed ("the rest of the stream is
# silent"), and the game left by WM_CLOSE.  Prints the voices per mode.
LOG="$1"
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -q 'TIMEOUT' "$LOG" && fail "a wait ran out: $(grep -m1 'TIMEOUT' "$LOG")"
grep -q 'EALayer3 decoder in SND' "$LOG" || fail "no EALayer3 decoder in SND's slot (a module without it, or NFSMW_EALAYER3=0)"
for m in 'front end done' 'free roam' 'free roam done' 'pursuit started' 'done'; do
  grep -q "LOG music_modes: $m\$" "$LOG" || fail "the script never reached: $m"
done
grep -q '\[el3\].*the rest of the stream is silent' "$LOG" && fail "a voice failed: $(grep -m1 'the rest of the stream is silent' "$LOG")"
# voices (channel 0 of a stream) before the free-roam mark = front end, up to the pursuit mark = free roam, after = pursuit
read -r fe roam purs <<< "$(awk '
  BEGIN { mode = 0 }
  /LOG music_modes: free roam$/ { mode = 1 }
  /LOG music_modes: pursuit started/ { mode = 2 }
  /\[el3\].* voice [0-9]+ channel 0 of/ { n[mode]++ }
  END { print n[0] + 0, n[1] + 0, n[2] + 0 }' "$LOG")"
[ "$fe" -ge 1 ] || fail "no music voice in the front end"
[ "$roam" -ge 1 ] || fail "no music voice in free roam"
[ "$purs" -ge 1 ] || fail "no music voice in the pursuit"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
echo "PASS: music through the EALayer3 decoder in the front end ($fe voices), free roam ($roam) and the pursuit ($purs)"
