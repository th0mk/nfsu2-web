#!/bin/bash
# Start NFS Underground 2 in the browser: one command on the Mac that hosts it.
#
#   web/start.sh [--data "<game folder>"] [--http-port 8080] [--open] [--no-build]
#   cd web && npm start -- [the same options]
#
# What it does, in order:
#   1. web/node_modules: installed (npm ci) if missing.
#   2. The game module (out/web/nfsu2.wasm): built with web/build.sh only when it
#      is missing or older than the runtime, the generated game code or the
#      build script.  A few seconds with the shared object cache.
#   3. The page (web/dist): built with node build.mjs only when older than its
#      sources.
#   4. The server (out/web/server-rs/nfsu2-server): built with
#      web/server-rs/build.sh only when it is missing or older than its sources
#      (it needs Rust: https://rustup.rs).
#   5. The server in the foreground, with the game folder: --data, else
#      $NFSU2_DATA, else the folder used last time, else
#      ~/projects/nsfu2/Need for Speed - Underground 2 if it exists.  Ctrl-C stops it.
# It prints the address for this Mac, http://localhost:<port>/; --open also opens
# it in Chrome.  The server listens on this Mac only: other PCs need HTTPS, from
# a certificate or a proxy or tunnel in front (docs/web/VPS.md).
# A build that fails stops here with its error (the last 40 lines; the whole log
# is in out/web/server/), instead of starting the server with a stale or missing
# module, page or server.
set -e -o pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
STATE="$ROOT/out/web/server"
SERVER=out/web/server-rs/nfsu2-server
DATA="" HTTP_PORT=8080 OPEN=0 BUILD=1
while [ $# -gt 0 ]; do
  case "$1" in
    --data) DATA="$2"; shift 2 ;;
    --http-port) HTTP_PORT="$2"; shift 2 ;;
    --open) OPEN=1; shift ;;
    --no-build) BUILD=0; shift ;;
    -h|--help) sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "start.sh: unknown option $1 (see --help)"; exit 2 ;;
  esac
done
say() { printf '\033[1m%s\033[0m\n' "$*"; }

# ---- the game folder ----------------------------------------------------------
if [ -z "$DATA" ]; then DATA="${NFSU2_DATA:-}"; fi
if [ -z "$DATA" ] && [ -f "$STATE/data-path" ]; then DATA="$(cat "$STATE/data-path")"; fi
if [ -z "$DATA" ] && [ -d "$HOME/projects/nsfu2/Need for Speed - Underground 2" ]; then DATA="$HOME/projects/nsfu2/Need for Speed - Underground 2"; fi
if [ -z "$DATA" ] || [ ! -f "$DATA/SPEED2.EXE" ]; then
  echo "Where is the game?  Start with the folder that holds SPEED2.EXE:"
  echo "  web/start.sh --data \"/path/to/Need for Speed - Underground 2\""
  [ -n "$DATA" ] && echo "(\"$DATA\" has no SPEED2.EXE)"
  exit 1
fi
mkdir -p "$STATE"; printf '%s' "$DATA" > "$STATE/data-path"

# ---- the port is free? -----------------------------------------------------------
if lsof -nP -iTCP:"$HTTP_PORT" -sTCP:LISTEN >/dev/null 2>&1; then
  echo "Port $HTTP_PORT is in use (by: $(lsof -nP -iTCP:"$HTTP_PORT" -sTCP:LISTEN | awk 'NR==2{print $1" pid "$2}')). Is the server already running?"
  echo "Open http://localhost:$HTTP_PORT/ if it is, or start on another port: web/start.sh --http-port 9080"
  exit 1
fi

if [ "$BUILD" = 1 ]; then
  # ---- 1. the page's tools ----
  if [ ! -d web/node_modules/esbuild ]; then
    say "Installing the page's tools (npm ci, once)"
    (cd web && npm ci --no-audit --no-fund)
  fi
  # ---- 2. the game module ----
  WASM=out/web/nfsu2.wasm
  stale=""
  if [ ! -f "$WASM" ]; then stale="missing"
  else
    newer="$(find -L runtime gen/all web/build.sh -newer "$WASM" -type f -print -quit 2>/dev/null || true)"
    [ -n "$newer" ] && stale="older than $newer"
  fi
  if [ -n "$stale" ]; then
    if ls -d deps/wasi-sdk-* >/dev/null 2>&1 && [ -f gen/all/fntab.c ]; then
      say "Building the game module ($stale)"
      # UG2's module, whatever GAME the shell has (web/build.sh builds another game's for GAME=mw)
      if ! GAME=ug2 web/build.sh > "$STATE/build.log" 2>&1; then
        tail -40 "$STATE/build.log"
        echo "The game module did not build (the whole log: $STATE/build.log). The server was not started."
        exit 1
      fi
      tail -3 "$STATE/build.log"
    elif [ -f "$WASM" ]; then
      echo "The game module is $stale, but this checkout cannot build it (no wasi-sdk in deps/ or no gen/all): using the one there is."
    else
      echo "No game module (out/web/nfsu2.wasm) and no toolchain to build it: see docs/web/PLAYING.md, 'Building'."
      exit 1
    fi
  fi
  # ---- 3. the page ----
  if [ ! -f web/dist/main.js ] || [ -n "$(find web/src web/index.html web/build.mjs app/registry-defaults.txt -newer web/dist/main.js -type f -print -quit 2>/dev/null || true)" ]; then
    say "Building the page"
    if ! (cd web && node build.mjs) > "$STATE/page-build.log" 2>&1; then
      tail -40 "$STATE/page-build.log"
      echo "The page did not build (the whole log: $STATE/page-build.log). The server was not started."
      exit 1
    fi
    tail -1 "$STATE/page-build.log"
  fi
  # ---- 4. the server ----
  if [ ! -x "$SERVER" ] || [ -n "$(find web/server-rs \( -name '*.rs' -o -name 'Cargo.*' -o -name '*.txt' \) -newer "$SERVER" -type f -print -quit 2>/dev/null || true)" ]; then
    say "Building the server"
    if ! web/server-rs/build.sh > "$STATE/server-build.log" 2>&1; then
      tail -40 "$STATE/server-build.log"
      echo "The server did not build (it needs Rust: https://rustup.rs; the whole log: $STATE/server-build.log). It was not started."
      exit 1
    fi
    tail -2 "$STATE/server-build.log"
  fi
fi
if [ ! -x "$SERVER" ]; then
  echo "No server ($SERVER): start without --no-build, or build it with web/server-rs/build.sh."
  exit 1
fi

# ---- 5. the server ----------------------------------------------------------------
say "Starting the server (Ctrl-C stops it)"
echo "  On this Mac:  http://localhost:$HTTP_PORT/"
if [ "$OPEN" = 1 ]; then
  ( for _ in $(seq 1 50); do curl -sf -o /dev/null "http://localhost:$HTTP_PORT/game.json" && break; sleep 0.2; done
    open -a "Google Chrome" "http://localhost:$HTTP_PORT/" 2>/dev/null || open "http://localhost:$HTTP_PORT/" ) &
fi
# UG2's server, whatever NFSU2_GAME the shell has: it is unset (an empty one is no game to the server), and no
# --game is passed (a server built before Most Wanted refuses it)
unset NFSU2_GAME
exec "$SERVER" --data "$DATA" --http-port "$HTTP_PORT"
