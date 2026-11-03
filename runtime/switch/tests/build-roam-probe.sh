#!/bin/bash
# Build roam_probe (fake free-roam friend) for Linux/WSL.
set -e
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="${OUT:-$HOME/nfsu2-linux-build}"
mkdir -p "$OUT"
# ws:// needs no TLS: stub the CA setter and the PEM symbol roam_probe references.
cat > "$OUT/n2n_tls_stub.c" <<'EOF'
const char nfsu2_cacert_pem[] = "";
void n2n_tls_set_ca(const char *pem){ (void)pem; }
EOF
cc -O1 -g -I"$ROOT/runtime" -I"$ROOT/homebrew/switch-net" \
  -o "$OUT/roam_probe" \
  "$ROOT/runtime/switch/tests/roam_probe.c" \
  "$ROOT/runtime/roam_core.c" \
  "$ROOT/homebrew/switch-net/n2n.c" \
  "$ROOT/homebrew/switch-net/n2n_sock.c" \
  "$OUT/n2n_tls_stub.c" \
  -lpthread -lm
echo "built $OUT/roam_probe"
