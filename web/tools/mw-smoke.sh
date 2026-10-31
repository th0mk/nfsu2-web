#!/bin/bash
# Most Wanted's release smoke (TRACKER M11-17): the release's server, page and module on this machine, then three runs
# in headless Chrome one after another, each checked by its tests/mw/checks/<name>.sh (run-suite prints PASS or FAIL):
#   boot_menu      boot to the main menu and quit by the window's close
#   race_win       a quick race, won over the line by the game's AI driving the player's car, and saved (a save
#                  folder with the aliases ALPHA and BRAVO: --seed, as tests/mw/scripts/profiles_make.txt leaves it)
#   career_start   a new career from its first screen to the first free drive
# (lan_host joins once Most Wanted has LAN races.)  About 2 minutes for the first two on a quiet machine; the career's
# prologue races take longer.  Nothing here reaches another machine: the server listens on 127.0.0.1 only.
#
#   web/tools/mw-smoke.sh --data "<NFS Most Wanted folder>" --seed <save folder with ALPHA and BRAVO> [--port 10730]
#        [--server <nfsu2-server>] [--d3dx <d3dx9_26.dll>] [--keep]
# --server: a release bundle's server (web/deploy/pack.sh, unpacked: its page and module beside it, NFSU2_REPO);
# default this checkout's (out/web/server-rs/nfsu2-server with web/dist-mw and out/web/mw/nfsmw.wasm).  The server runs
# with --dev, which run-suite's test page needs (it is not the players' page: the release's page and module are what
# it loads).  --d3dx: a module built with the D3DX oracle needs the DLL in each run's save folder.  CHROME_ARGS: the
# browser's switches (WSL's Chrome: the SwiftShader ones, docs/web).  Exit 0 when all three pass.
set -u -o pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
DATA="" SEED="" PORT=10730 SERVER="$REPO/out/web/server-rs/nfsu2-server" D3DX="" KEEP=0
while [ $# -gt 0 ]; do
  case "$1" in
    --data) DATA="$2"; shift 2 ;;
    --seed) SEED="$2"; shift 2 ;;
    --port) PORT="$2"; shift 2 ;;
    --server) SERVER="$2"; shift 2 ;;
    --d3dx) D3DX="$2"; shift 2 ;;
    --keep) KEEP=1; shift ;;
    -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
    *) echo "mw-smoke.sh: unknown option $1"; exit 2 ;;
  esac
done
[ -n "$DATA" ] && [ -f "$DATA/speed.exe" ] || { echo "mw-smoke.sh: --data \"<the folder with speed.exe>\""; exit 2; }
[ -n "$SEED" ] && [ -d "$SEED/NFS Most Wanted/ALPHA" ] || { echo "mw-smoke.sh: --seed <a save folder with NFS Most Wanted/ALPHA>"; exit 2; }
[ -x "$SERVER" ] || { echo "mw-smoke.sh: no server at $SERVER (web/server-rs/build.sh)"; exit 2; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/mw-smoke.XXXXXX")"
RUN="mw-smoke-$(date +%Y%m%d-%H%M%S)"
SRV_PID=""
cleanup() { [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null; wait "$SRV_PID" 2>/dev/null; [ "$KEEP" = 1 ] || rm -rf "$WORK"; }
trap cleanup EXIT
# the release's server: its page and module are the ones beside it (a bundle) or this checkout's
REL="$(cd "$(dirname "$SERVER")" && pwd)"
if [ "$SERVER" = "$REPO/out/web/server-rs/nfsu2-server" ]; then ROOT="$REPO"; else ROOT="$REL"; fi
NFSU2_REPO="$ROOT" "$SERVER" --game mw --dev --data "$DATA" --http-port "$PORT" --state "$WORK/state" \
  >"$WORK/server.log" 2>&1 &
SRV_PID=$!
for _ in $(seq 1 120); do
  curl -fsS -o /dev/null "http://localhost:$PORT/mw/game.json" 2>/dev/null && break
  kill -0 "$SRV_PID" 2>/dev/null || { echo "mw-smoke.sh: the server stopped:"; tail -5 "$WORK/server.log"; exit 1; }
  sleep 1
done
curl -fsS -o /dev/null "http://localhost:$PORT/mw/game.json" || { echo "mw-smoke.sh: no /mw/game.json in 2 minutes"; exit 1; }
# each run's save folder: the seed (race_win), or only the DLL of an oracle build (boot_menu, career_start: a first start)
mkdir -p "$WORK/empty"
if [ -n "$D3DX" ]; then cp "$D3DX" "$WORK/empty/"; cp -r "$SEED" "$WORK/seed"; cp "$D3DX" "$WORK/seed/"; else cp -r "$SEED" "$WORK/seed"; fi
DLL_ENV=(); [ -n "$D3DX" ] && DLL_ENV=(--env NFSMW_D3DX=/save/d3dx9_26.dll)
fails=0
run() {  # name seed marker env...
  local name="$1" seed="$2" marker="$3"; shift 3
  local out
  out=$(NFSU2_SAVE_SEED="$seed" node "$REPO/web/tools/run-suite.mjs" --game mw --base "http://localhost:$PORT/mw" --run "$RUN-$name" \
    --script "$name.txt" --marker "$marker" "${DLL_ENV[@]}" "$@" 2>&1)
  local line; line=$(printf '%s\n' "$out" | grep -E '^(PASS|FAIL)' | tail -1)
  echo "${line:-FAIL $name: no result (out/web/runs/$RUN-$name)}"
  case "$line" in PASS*) ;; *) fails=$((fails + 1)) ;; esac
}
t0=$(date +%s)
run boot_menu "$WORK/empty" 'waitfng MainMenu.fng satisfied' --env NFSU2_QUIT_FRAMES=7200
run race_win "$WORK/seed" 'race_win: ALPHA saved' --env FILELIST=1 --env NFSU2_QUIT_FRAMES=60000 --timeout-scale 3
run career_start "$WORK/empty" 'career_start: drove' --env FILELIST=1 --env NFSU2_QUIT_FRAMES=150000 --timeout-scale 4
echo "mw-smoke: $((3 - fails))/3 passed in $(( $(date +%s) - t0 )) s (runs: out/web/runs/$RUN-*)"
[ "$fails" = 0 ]
