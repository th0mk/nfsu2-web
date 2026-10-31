#!/bin/bash
# Suite check for "pad_binds" (tests/mw/scripts/pad_binds.txt, TRACKER M6-37; pad_binds_wheel.sh runs it with
# KIND=wheel): the module under test is named (L-020); every wait in time; the joystick's first binding of each row
# runtime/mw/pad_mw.c writes is its layout's (type, joystick 0, code) and the second is none; ENGAGE_EVENT keeps the
# game's D-pad up.  Rows (GAME.md 13.1): 0 GAS, 1 BRAKE, 2 STEERLEFT, 3 STEERRIGHT, 4 HANDBRAKE, 5 GAMEBREAKER, 6 NOS,
# 7 SHIFTDOWN, 8 SHIFTUP, 9 RESET, 10 ENGAGE_EVENT, 15 CAM_ACTION_CHANGE, 16 LOOKBACK, 18 PAUSEREQUEST.  Types: 2 a
# button (X 0, A 1, B 2, Y 3, LB 4, RB 5, View 8, Menu 9, RS 11), 3 a POV direction, 4/5 X, Y, Z below/above the
# centre (code 0 X, 2 Z: the left trigger), 7 Rz above it (the right trigger).
LOG="$1"
KIND="${KIND:-pad}"
NAME=pad_binds; [ "$KIND" = wheel ] && NAME=pad_binds_wheel
fail(){ echo "$1"; exit 1; }
grep -qE '^module: rev [0-9a-f]+\+?, game mw, .* stamp game mw;' "$LOG" || fail "no module line naming its revision and lift stamp"
grep -qE '^\[autotest\] f=[0-9]+ .* TIMEOUT' "$LOG" && fail "a wait of the script ran out: $(grep -m1 -E 'TIMEOUT' "$LOG")"
grep -qF "LOG $NAME: done" "$LOG" || fail "no 'LOG $NAME: done' line"
#        row type code
WANT="0 7 2  1 5 2  2 4 0  3 5 0  4 2 5  5 2 11  6 2 4  7 2 0  8 2 1  9 2 8  10 3 0  15 2 3  16 2 2  18 2 9"
[ "$KIND" = wheel ] && WANT="0 7 2  1 5 2  2 4 0  3 5 0  4 2 0  5 2 11  6 2 1  7 2 4  8 2 5  9 2 8  10 3 0  15 2 3  16 2 2  18 2 9"
bad=$(awk -v want="$WANT" '
  / peek \[0x[0-9a-fA-F]+\] = 0x/ { a = $0; sub(/.* peek \[/, "", a); v = a; a = substr(a, 1, 10); sub(/.*\] = /, "", v); v = substr(v, 1, 10)
                                   m[strtonum(a)] = strtonum(v) }
  END { n = split(want, w, " +"); out = ""
        for (i = 1; i + 2 <= n; i += 3) {
          r = w[i]; at = 9565208 + r * 52 + 28            # 0x91F418 + row * 0x34 + 0x1C
          if (!((at) in m) || !((at + 8) in m) || !((at + 12) in m)) { out = out " row " r ": not peeked"; continue }
          if (m[at] != w[i + 1] || m[at + 4] != 0 || m[at + 8] != w[i + 2] || (r != 10 && m[at + 12] != 0))
            out = out sprintf(" row %d: (%d,%d,%d) second type %d, want (%d,0,%d)", r, m[at], m[at + 4], m[at + 8], m[at + 12], w[i + 1], w[i + 2])
        }
        print out }' "$LOG")
[ -n "$bad" ] && fail "binding table:$bad"
grep -qF 'app: the game exited (code 0) after the quit request' "$LOG" || fail "the game did not exit after WM_CLOSE"
echo "the $KIND's rows are pad_mw.c's layout"
