#!/bin/bash
# The lifts made on this machine, kept by what they were made from, so that the same lift is made once:
# ~/.cache/nsfu2-web/gen/<key>/{all,server}, key = a hash of tools/genstamp.sh's source lines (override list, hook
# list, lifter).  Only web/gen.sh's default lift (no WEB_LIFT, JTSWITCH, LREGS, FTOL2INL or INLINE_LIST set) is
# kept; the newest 12 stay.  The kept lifts are never written to again: build against them through links.
#
#   web/tools/gencache.sh link          gen/all and gen/server are links (a worktree's): point them at the kept lift
#                                       for this checkout's sources, if there is one and they show another
#                                       (web/build.sh runs this first)
#   web/tools/gencache.sh get <exe>     gen/all and gen/server are folders: copy the kept lift for this checkout
#                                       into them, exit 1 when there is none (web/gen.sh runs this first)
#   web/tools/gencache.sh put <exe>     keep gen/all and gen/server (web/gen.sh runs this when it has finished)
#   web/tools/gencache.sh add <dir>     keep <dir>/all and <dir>/server, a finished default lift made elsewhere
#   web/tools/gencache.sh key|dir       this checkout's key, the kept lifts' folder
set -e -o pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
CACHE="$HOME/.cache/nsfu2-web/gen"
DEFAULT_LIFT="FLAGABI X87OPT JTSWITCH LREGS FTOL2INL INLINE_LIST"
WANT="$(tools/genstamp.sh sources)"
KEY="$(printf '%s\n' "$WANT" | shasum -a 256 | cut -c1-16)"
sha(){ shasum -a 256 < "$1" | cut -c1-16; }
# <dir>/all and <dir>/server: a finished lift of these sources (any options), both from one run
current(){ [ -f "$1/all/fntab.c" ] && [ -f "$1/server/fntab.c" ] &&
           [ "$(grep -E '^(overrides|hooks|lifter) ' "$1/all/stamp.txt" 2>/dev/null || true)" = "$WANT" ] &&
           cmp -s "$1/all/stamp.txt" "$1/server/stamp.txt"; }
# ... and web/gen.sh's default one
fits(){ current "$1" && [ "$(cat "$1/all/web-lift.txt" 2>/dev/null)" = "$DEFAULT_LIFT" ]; }
kept(){ [ -f "$CACHE/$KEY/done" ] && fits "$CACHE/$KEY"; }
keep(){   # $1: folder with all/ and server/ (a finished default lift of any sources), $2: the exe's hash (or empty)
  local lines k t
  lines="$(grep -E '^(overrides|hooks|lifter) ' "$1/all/stamp.txt" 2>/dev/null || true)"
  [ "$(printf '%s\n' "$lines" | grep -c .)" = 3 ] && [ -f "$1/all/fntab.c" ] && [ -f "$1/server/fntab.c" ] &&
    [ "$(cat "$1/all/web-lift.txt" 2>/dev/null)" = "$DEFAULT_LIFT" ] && cmp -s "$1/all/stamp.txt" "$1/server/stamp.txt" ||
    { echo "gencache: $1 is not a finished default lift: not kept"; return 0; }
  k="$(printf '%s\n' "$lines" | shasum -a 256 | cut -c1-16)"
  [ ! -f "$CACHE/$k/done" ] || return 0
  mkdir -p "$CACHE"; t="$(mktemp -d "$CACHE/.new-XXXXXX")"
  cp -R "$1/all" "$t/all"; cp -R "$1/server" "$t/server"
  [ -z "$2" ] || echo "$2" > "$t/exe.sha"
  touch "$t/done"
  if [ -e "$CACHE/$k" ]; then rm -rf "$t"; else mv "$t" "$CACHE/$k"; echo "gencache: kept as $CACHE/$k"; fi
  ls -1dt "$CACHE"/*/ | tail -n +13 | while read -r d; do rm -rf "$d"; done
  find "$CACHE" -maxdepth 1 -name '.new-*' -mmin +120 -exec rm -rf {} + 2>/dev/null || true
}
case "${1:-}" in
  key) echo "$KEY" ;;
  dir) echo "$CACHE" ;;
  link)
    for d in gen/all gen/server; do [ -L "$d" ] || [ ! -e "$d" ] || exit 0; done   # a folder: the checkout's own lift
    current gen && exit 0
    kept || exit 0
    mkdir -p gen
    ln -sfn "$CACHE/$KEY/all" gen/all; ln -sfn "$CACHE/$KEY/server" gen/server
    touch "$CACHE/$KEY"
    echo "gen/all, gen/server -> $CACHE/$KEY (a lift of these sources made before)" ;;
  get)
    EXE="${2:?usage: gencache.sh get <exe>}"
    kept || exit 1
    [ ! -f "$CACHE/$KEY/exe.sha" ] || [ "$(cat "$CACHE/$KEY/exe.sha")" = "$(sha "$EXE")" ] || exit 1
    mkdir -p gen
    for d in all server; do rm -rf "gen/$d"; cp -R "$CACHE/$KEY/$d" "gen/$d"; done
    touch "$CACHE/$KEY"
    echo "gen/all, gen/server: copied from $CACHE/$KEY (a lift of these sources made before)" ;;
  put) keep gen "$(sha "${2:?usage: gencache.sh put <exe>}")" ;;
  add) keep "${2:?usage: gencache.sh add <dir>}" "" ;;
  *) sed -n '2,15p' "$0"; exit 2 ;;
esac
