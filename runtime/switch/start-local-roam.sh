#!/bin/bash
# Local free-roam hub for yuzu: server + N bots near the career spawn (1695, -20-ish).
#   bash runtime/switch/start-local-roam.sh [friends=2] [http_port=8765]
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
N="${1:-2}"
PORT="${2:-8765}"
STATE=/tmp/nfsu2-yuzu-roam-state
DATA="${NFSU2_GAME_DIR:?set NFSU2_GAME_DIR to the folder with SPEED2.EXE}"
BIN="$ROOT/out/web/server-rs/nfsu2-server"
LOAD="$ROOT/out/web/server-rs/nfsu2-roamload"
PROBE="$HOME/nfsu2-linux-build/roam_probe"
. "$HOME/.cargo/env" 2>/dev/null || true
pkill -f "nfsu2-server --data" 2>/dev/null || true
pkill -f nfsu2-roamload 2>/dev/null || true
pkill -x roam_probe 2>/dev/null || true
sleep 1
rm -rf "$STATE"; mkdir -p "$STATE"
# HTTP on 127.0.0.1 — Windows yuzu reaches WSL via localhost forwarding.
nohup "$BIN" --data "$DATA" --http-port "$PORT" --state "$STATE" --dev >"$STATE/server.log" 2>&1 &
echo $! >"$STATE/server.pid"
for i in $(seq 1 50); do
  if grep -q "listening\|http" "$STATE/server.log" 2>/dev/null; then break; fi
  # also try curl
  if curl -sf "http://127.0.0.1:$PORT/" >/dev/null 2>&1; then break; fi
  sleep 0.2
done
# roamload places bots around (0,0) — far from career spawn. Use roam_probe near airport instead.
if [ "$N" -gt 0 ]; then
  if [ ! -x "$PROBE" ]; then
    echo "missing $PROBE — run runtime/switch/tests/build-roam-probe.sh"; exit 1
  fi
  i=0
  while [ "$i" -lt "$N" ]; do
    # ring ~25 m around SPAWN (1695.3, -883.6, 24.1), outside at_spawn (5 m)
    x=$(awk -v i="$i" 'BEGIN{printf "%.1f", 1695.3 + 25*cos(i*1.7)}')
    y=$(awk -v i="$i" 'BEGIN{printf "%.1f", -883.6 + 25*sin(i*1.7)}')
    nohup "$PROBE" "ws://127.0.0.1:$PORT/lan" "Bot$i" "$x" "$y" 24.1 600 16 \
      >"$STATE/probe$i.log" 2>&1 &
    echo $! >"$STATE/probe$i.pid"
    i=$((i+1))
  done
fi
sleep 3
echo "server pid $(cat $STATE/server.pid) port $PORT probes=$N"
tail -8 "$STATE/server.log" || true
shopt -s nullglob
for f in "$STATE"/probe*.log; do
  echo "--- $f"; tail -6 "$f" || true
done
exit 0
