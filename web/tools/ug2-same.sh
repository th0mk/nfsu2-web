#!/bin/bash
# UG2-same: is a UG2 lift made from this checkout's sources byte for byte the
# reviewed baseline?  Lifter and runtime changes made for another game must
# leave UG2's gen/all and gen/server as they are.
#
#   web/tools/ug2-same.sh path/to/SPEED2.EXE             lift UG2 from this checkout and compare
#   web/tools/ug2-same.sh --no-copy path/to/SPEED2.EXE   lift the copy the last run left, as it is
#                                                        (a change planted there tests this check)
#   UG2_BASE=<dir> web/tools/ug2-same.sh <exe>           compare with <dir>/{all,server}
#
# The lift runs in a copy, never in this checkout's gen/: the tracked and new
# non-ignored files (git ls-files -co --exclude-standard) go to
# out/mw/ug2-same/tree, which links deps/ and .venv as web/tools/mkworktree.sh
# does and has its own gen/all and gen/server.  web/gen.sh runs there in a
# clean environment with the baseline's options (all/web-lift.txt and the
# stamp's options line); server.dll is taken from next to the exe, as always.
# The baseline (default out/mw/ug2-base) is a fresh UG2 lift of main's head
# made with main's tools.
#
# Compared: every file of all/ and server/ except stamp.txt, byte for byte.  A
# file only the new lift has passes when web/tools/ug2-same.allow names it.
# The stamps: the date line is ignored; exe, options, overrides and hooks must
# match.  Another lifter hash, or another flag table hash (FLAGABI= in the
# options line), is printed as information: the output is what must not change.
# Exit 0: identical.  1: different (the files are named).  2: no comparison
# (usage, baseline, exe, or the lift failed; out/mw/ug2-same/gen.log).
set -e -o pipefail
die(){ echo "ug2-same: $*" >&2; exit 2; }
abs(){ case "$1" in /*) echo "$1" ;; *) echo "$PWD/$1" ;; esac; }
NOCOPY=0; [ "${1:-}" = --no-copy ] && { NOCOPY=1; shift; }
EXE="${1:-${NFSU2_DATA:+$NFSU2_DATA/SPEED2.EXE}}"
[ -f "$EXE" ] || die "usage: web/tools/ug2-same.sh [--no-copy] path/to/SPEED2.EXE"
EXE="$(abs "$EXE")"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BASE="$(abs "${UG2_BASE:-$ROOT/out/mw/ug2-base}")"
cd "$ROOT"
WORK="$ROOT/out/mw/ug2-same"; TREE="$WORK/tree"; ALLOW="$ROOT/web/tools/ug2-same.allow"
for f in all/stamp.txt server/stamp.txt all/web-lift.txt; do
  [ -f "$BASE/$f" ] || die "no $BASE/$f: make the baseline first (a fresh web/gen.sh lift of main's head, copied there)"
done
cmp -s "$BASE/all/stamp.txt" "$BASE/server/stamp.txt" || die "$BASE: all/stamp.txt and server/stamp.txt differ (not one lift)"
h16(){ shasum -a 256 | cut -c1-16; }
WANT_EXE=$(sed -nE 's/^exe ([0-9a-f]+).*/\1/p' "$BASE/all/stamp.txt")
[ "$(h16 < "$EXE")" = "$WANT_EXE" ] || die "$EXE is not the exe the baseline was lifted from (exe $WANT_EXE)"

# the baseline's options: web/gen.sh's from web-lift.txt, the rest from the stamp
OPTS="$(cat "$BASE/all/web-lift.txt")"
has(){ case " $OPTS " in *" $1 "*) echo 1 ;; *) echo 0 ;; esac; }
if [ "$OPTS" = plain ]; then LIFTENV=(WEB_LIFT=plain)
else
  LIFTENV=(JTSWITCH="$(has JTSWITCH)" LREGS="$(has LREGS)" FTOL2INL="$(has FTOL2INL)")
  [ "$(has INLINE_LIST)" = 1 ] || LIFTENV+=(INLINE_LIST=)   # on: web/gen.sh's own list, from the copy
fi
for kv in $(sed -n 's/^options //p' "$BASE/all/stamp.txt"); do
  k="${kv%%=*}"; v="${kv#*=}"
  case "$k" in
    FLAGABI|X87OPT|JTSWITCH|LREGS|FTOL2INL|INLINE_LIST) ;;   # set by web/gen.sh from the above
    NSHARD) LIFTENV+=(NSHARD="${v%/*}" NSHARD_SRV="${v#*/}") ;;
    *) if [ -n "$v" ]; then LIFTENV+=("$k=$v"); fi ;;
  esac
done
KEEP=(PATH="$PATH" HOME="$HOME")
for v in TMPDIR LANG LC_ALL LC_CTYPE; do if [ -n "${!v:-}" ]; then KEEP+=("$v=${!v}"); fi; done

mkdir -p "$WORK"
mkdir "$WORK/lock" 2>/dev/null || die "another ug2-same run holds $WORK/lock (remove it if none is running)"
trap 'rmdir "$WORK/lock"' EXIT
T0=$(date +%s)
if [ $NOCOPY = 0 ]; then
  rm -rf "$TREE"; mkdir -p "$TREE"
  git -C "$ROOT" ls-files -co --exclude-standard -z | while IFS= read -r -d '' f; do
    if [ -e "$f" ] || [ -L "$f" ]; then printf '%s\0' "$f"; fi   # a tracked file deleted here is left out
  done | COPYFILE_DISABLE=1 tar --null -T - -cf - | tar -xf - -C "$TREE"
  for l in deps .venv; do if [ -e "$ROOT/$l" ]; then ln -s "$ROOT/$l" "$TREE/$l"; fi; done
else
  [ -f "$TREE/web/gen.sh" ] || die "--no-copy: no copy in $TREE (run once without --no-copy)"
fi
rm -rf "$TREE/gen" "$TREE/out"; mkdir -p "$TREE/gen/all" "$TREE/gen/server"
echo "ug2-same: lifting UG2 in $TREE ($OPTS)"
( cd "$TREE" && env -i "${KEEP[@]}" "${LIFTENV[@]}" ./web/gen.sh "$EXE" ) > "$WORK/gen.log" 2>&1 \
  || { tail -15 "$WORK/gen.log" >&2; die "web/gen.sh failed in the copy ($WORK/gen.log)"; }

DIFF=(); INFO=(); NEWOK=()
ALLOWED="$(grep -vE '^[[:space:]]*(#|$)' "$ALLOW" 2>/dev/null | sed 's/[[:space:]]*$//' || true)"
files(){ ( cd "$1" && find . -type f ! -name stamp.txt | sed 's|^\./||' | LC_ALL=C sort ); }
for d in all server; do
  NEW="$(files "$TREE/gen/$d")"; OLD="$(files "$BASE/$d")"
  while IFS= read -r f; do [ -z "$f" ] || DIFF+=("missing  $d/$f"); done < <(LC_ALL=C comm -13 <(echo "$NEW") <(echo "$OLD"))
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    if grep -qxF "$d/$f" <<< "$ALLOWED"; then NEWOK+=("$d/$f"); else DIFF+=("new      $d/$f"); fi
  done < <(LC_ALL=C comm -23 <(echo "$NEW") <(echo "$OLD"))
  while IFS= read -r f; do
    [ -z "$f" ] || cmp -s "$TREE/gen/$d/$f" "$BASE/$d/$f" || DIFF+=("differs  $d/$f")
  done < <(LC_ALL=C comm -12 <(echo "$NEW") <(echo "$OLD"))
done
S_NEW="$TREE/gen/all/stamp.txt"; S_OLD="$BASE/all/stamp.txt"
cmp -s "$S_NEW" "$TREE/gen/server/stamp.txt" || DIFF+=("stamp    server/stamp.txt is not the one of all/stamp.txt")
flagabi(){ sed -E 's/ FLAGABI=[0-9a-f]+/ FLAGABI=on/'; }
for k in $( { cut -d' ' -f1 "$S_NEW"; cut -d' ' -f1 "$S_OLD"; } | grep -v '^#' | LC_ALL=C sort -u); do
  a="$(grep -m1 "^$k " "$S_NEW" || true)"; b="$(grep -m1 "^$k " "$S_OLD" || true)"
  [ "$a" != "$b" ] || continue
  case "$k" in
    lifter) INFO+=("lifter ${b#lifter } (baseline) -> ${a#lifter } (this lift)") ;;
    options)
      if [ "$(flagabi <<< "$a")" = "$(flagabi <<< "$b")" ]; then
        INFO+=("flag table $(sed -nE 's/.* FLAGABI=([^ ]+).*/\1/p' <<< "$b") (baseline) -> $(sed -nE 's/.* FLAGABI=([^ ]+).*/\1/p' <<< "$a")")
      else DIFF+=("stamp    options: baseline '${b#options }', this lift '${a#options }'"); fi ;;
    *) DIFF+=("stamp    $k: baseline '${b:-none}', this lift '${a:-none}'") ;;
  esac
done

MIN=$(awk -v s=$(( $(date +%s) - T0 )) 'BEGIN { printf "%.1f", s / 60 }')
for i in "${INFO[@]}"; do echo "  info: $i"; done
for f in "${NEWOK[@]}"; do echo "  new (allowed in web/tools/ug2-same.allow): $f"; done
if [ ${#DIFF[@]} = 0 ]; then
  echo "ug2-same: identical to $BASE ($(files "$BASE/all" | grep -c .) + $(files "$BASE/server" | grep -c .) files and the stamp; $MIN min)"
  exit 0
fi
echo "ug2-same: DIFFERENT from $BASE ($MIN min):"
for l in "${DIFF[@]}"; do echo "  $l"; done
echo "  (${#DIFF[@]} differences; diff -r $TREE/gen/all $BASE/all, and the same for server, shows them)"
exit 1
