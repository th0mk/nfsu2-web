#!/bin/bash
# Builds the bundle that runs the server on a VPS without Docker (docs/web/VPS.md, "Without Docker"):
#
#   web/deploy/pack.sh [--out <dir>] [--no-build] [--data "<game folder>"] [--arch amd64|arm64] [--server <binary>]
#
# out/web/deploy/nfsu2-web-<rev>-<arch>.tar.gz holds nfsu2-web/ with
#   nfsu2-server      the server: one static binary for Linux on --arch (default amd64), built in Docker by
#                     web/server-rs/Dockerfile's binary stage (as the image's, cross-compiled: no emulation), or
#                     --server's (a Linux binary of your own, such as /app/nfsu2-server out of the image)
#   web/dist/         the page (the test pages and source maps are in web/dist-dev, never copied)
#   out/web/          the game module (nfsu2.wasm, nfsu2.json), built here with web/build.sh
#   deploy/           nfsu2-web.service (systemd), Caddyfile (an example), cloudflared.yml (an example),
#                     cloudflare-origin-pull-ca.pem (for Caddy behind Cloudflare's proxy)
#   README.md         the short version of docs/web/VPS.md
# The server finds the page and the module in the folder NFSU2_REPO names (the unit sets /opt/nfsu2-web).
# The bundle NEVER holds the game folder or anything from it: the files are copied from a
# fixed list, and the bundle is refused if any file in it has the name and size
# of a file in the game folder (--data, else $NFSU2_DATA, when it is there),
# carries the game's file names, or names this machine (host name, home folder).
# The archive's owner is root, its modes 644/755, without macOS metadata.
# --no-build: use web/dist as it is (default: rebuild it with node build.mjs).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/out/web/deploy" BUILD=1 DATA="${NFSU2_DATA:-}" ARCH=amd64 SERVER=""
while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$2"; shift 2 ;;
    --no-build) BUILD=0; shift ;;
    --data) DATA="$2"; shift 2 ;;
    --arch) ARCH="$2"; shift 2 ;;
    --server) SERVER="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; shift 2 ;;
    -h|--help) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "pack.sh: unknown option $1"; exit 2 ;;
  esac
done
case "$ARCH" in amd64|arm64) ;; *) echo "pack.sh: --arch amd64 or arm64, not $ARCH"; exit 2 ;; esac
[ -z "$DATA" ] && [ -d "$HOME/projects/nsfu2/Need for Speed - Underground 2" ] && DATA="$HOME/projects/nsfu2/Need for Speed - Underground 2"
cd "$ROOT"
REV="$(git rev-parse --short HEAD)$(git diff --quiet HEAD -- web/server-rs web/src web/index.html 2>/dev/null || echo +)"
[ -f out/web/nfsu2.wasm ] && [ -f out/web/nfsu2.json ] || { echo "no game module: build it first (web/build.sh)"; exit 1; }
if [ "$BUILD" = 1 ]; then (cd web && node build.mjs >/dev/null 2>&1) || { echo "the page did not build (cd web && node build.mjs)"; exit 1; }; fi

STAGE="$OUT/stage"
rm -rf "$STAGE"; mkdir -p "$STAGE/nfsu2-web"
B="$STAGE/nfsu2-web"
mkdir -p "$B/out/web" "$B/deploy"
# ---- the server ----
if [ -n "$SERVER" ]; then
  cp "$SERVER" "$B/nfsu2-server"
else
  command -v docker >/dev/null || { echo "no docker to build the server in: install it, or give one with --server"; exit 1; }
  echo "building the server for linux/$ARCH (docker build --target binary; the first build takes a few minutes)"
  docker build -q -f web/server-rs/Dockerfile --platform "linux/$ARCH" --target binary --output "type=local,dest=$STAGE/bin" . >/dev/null \
    || { echo "the server did not build (docker build -f web/server-rs/Dockerfile --target binary)"; exit 1; }
  mv "$STAGE/bin/nfsu2-server" "$B/nfsu2-server"; rm -rf "$STAGE/bin"
fi
[ "$(head -c 4 "$B/nfsu2-server" | od -An -c | tr -d ' ')" = '177ELF' ] || { echo "pack.sh: the server (${SERVER:-from docker build}) is no Linux binary"; exit 1; }
# ---- the fixed list ----
# the page: everything web/dist has (node build.mjs puts the test pages and source maps in web/dist-dev;
# they are left out here too, should an older build have left them in web/dist)
(cd web/dist && find . -type f ! -path './test/*' ! -name '*.map' -print) | while read -r f; do
  mkdir -p "$B/web/dist/$(dirname "$f")"; cp "web/dist/$f" "$B/web/dist/$f"
done
cp out/web/nfsu2.wasm out/web/nfsu2.json "$B/out/web/"
cp web/deploy/nfsu2-web.service web/deploy/Caddyfile web/deploy/cloudflared.yml "$B/deploy/"
cp web/server-rs/crates/headers/cloudflare-origin-pull-ca.pem "$B/deploy/"
cp web/deploy/README.md "$B/README.md"

# ---- refusals ----
fail() { echo "pack.sh: REFUSED: $*"; rm -rf "$STAGE"; exit 1; }
[ -z "$(find "$B" -type l -print -quit)" ] || fail "a symbolic link in the bundle"
GAME_NAMES='(^|/)SPEED2\.EXE$|\.(BUN|BIN|LZC|VIV|MUS|ASF|VP6|ABK|GIN|CSI|MPF|AUD|LZW|DLL|EXE)$'
bad="$(cd "$B" && find . -type f | grep -iE "$GAME_NAMES" || true)"
[ -z "$bad" ] || fail "game file names: $bad"
if [ -n "$DATA" ] && [ -d "$DATA" ]; then
  # name + size of every game file against every bundle file
  node -e '
    const fs = require("fs"), path = require("path");
    const walk = (d, out = []) => { for (const e of fs.readdirSync(d, { withFileTypes: true })) { const p = path.join(d, e.name);
      if (e.isDirectory()) walk(p, out); else if (e.isFile()) out.push(p); } return out; };
    const game = new Set(walk(process.argv[1]).map(p => path.basename(p).toLowerCase() + ":" + fs.statSync(p).size));
    const hit = walk(process.argv[2]).filter(p => game.has(path.basename(p).toLowerCase() + ":" + fs.statSync(p).size));
    if (hit.length) { console.log(hit.join("\n")); process.exit(1); }' "$DATA" "$B" || fail "files that match the game folder"
  echo "checked against the game folder: no file of it is in the bundle"
  # what the server leaves out of the game folder (the game's entries, as the server prints them: the bundle's own
  # server where this machine runs it, else the one web/server-rs/build.sh built here; the bundle is NFS Underground
  # 2's): copy only the rest to the VPS
  entries="$("$B/nfsu2-server" --game ug2 --print-entries 2>/dev/null || out/web/server-rs/nfsu2-server --game ug2 --print-entries 2>/dev/null || true)"
  if [ -z "$entries" ]; then
    echo "NOTE: the list of the game's own files was not read: the bundle's server does not run on this machine, nor does out/web/server-rs/nfsu2-server (web/server-rs/build.sh)"
  else
    others="$(node -e '
      const fs = require("fs");
      const keep = new Set(process.argv[2].split("\n").filter(Boolean));
      console.log(fs.readdirSync(process.argv[1]).filter(n => !n.startsWith(".") && !keep.has(n.toLowerCase())).join(", "));' "$DATA" "$entries")"
    [ -z "$others" ] || echo "NOTE: the server serves only the game's own files; these of your game folder it never serves (leave them off the VPS): $others"
  fi
fi
HOSTNAME_SHORT="$(hostname | sed 's/\.local$//')"
for needle in "$HOME" "$HOSTNAME_SHORT"; do
  hit="$(grep -rlaF "$needle" "$B" || true)"
  [ -z "$hit" ] || fail "names this machine ($needle) in: $hit"
done
SIZE_MB=$(( $(du -sk "$B" | cut -f1) / 1024 ))
[ "$SIZE_MB" -lt 200 ] || fail "$SIZE_MB MB: more than the server, the page and the module"

# ---- the archive ----
find "$B" -type d -exec chmod 755 {} +; find "$B" -type f -exec chmod 644 {} +; chmod 755 "$B/nfsu2-server"
mkdir -p "$OUT"
TGZ="$OUT/nfsu2-web-$REV-$ARCH.tar.gz"
# owner root, no extended attributes: bsdtar (macOS) and GNU tar (Linux, WSL) spell it differently
if tar --version 2>/dev/null | grep -q bsdtar; then
  TAR_OWNER=(--no-mac-metadata --no-xattrs --uid 0 --gid 0 --uname root --gname root)
else
  TAR_OWNER=(--no-xattrs --owner=root:0 --group=root:0)
fi
COPYFILE_DISABLE=1 tar -C "$STAGE" "${TAR_OWNER[@]}" -czf "$TGZ" nfsu2-web
rm -rf "$STAGE"
echo "$TGZ"
echo "  $(du -h "$TGZ" | cut -f1), $(tar -tzf "$TGZ" | grep -vc '/$') files, sha256 $(shasum -a 256 "$TGZ" | cut -d' ' -f1)"
