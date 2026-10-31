#!/usr/bin/env bash
# Only Cloudflare's addresses reach the server's TLS port (compose.origin.yml, docs/web/VPS.md "Cloudflare's proxy,
# straight to the server"): a table of its own in nftables, so nothing else on the VPS changes (Docker's rules, ufw's,
# other ports).  The server also asks Cloudflare's addresses for its origin-pull certificate; this keeps everyone
# else from reaching it past Cloudflare.  Run as root; again when Cloudflare's list changes (www.cloudflare.com/ips).
#
#   web/deploy/cloudflare-only.sh [port]          the port (default 28443) for Cloudflare's addresses only
#   web/deploy/cloudflare-only.sh --off           the table gone: the port open to all again
#
# At boot: nfsu2-cloudflare-only.service (next to this file) runs it from /srv/nfsu2.
set -euo pipefail
table=nfsu2_cloudflare
if [ "${1:-}" = --off ]; then
  nft delete table inet "$table" 2>/dev/null || true
  echo "cloudflare-only: off"
  exit 0
fi
port="${1:-28443}"
case "$port" in '' | *[!0-9]*) echo "cloudflare-only: not a port: $port" >&2; exit 1 ;; esac
list() { curl -fsS --max-time 20 "https://www.cloudflare.com/ips-$1" | tr -d '\r' | sed '/^$/d' | paste -sd, -; }
v4=$(list v4) v6=$(list v6)
[ -n "$v4" ] && [ -n "$v6" ] || { echo "cloudflare-only: no list from www.cloudflare.com" >&2; exit 1; }
# one transaction: the table made if it is not there, then dropped and made again, so a second run replaces the first
nft -f - <<EOF
table inet $table
delete table inet $table
table inet $table {
  set v4 { type ipv4_addr; flags interval; elements = { $v4 } }
  set v6 { type ipv6_addr; flags interval; elements = { $v6 } }
  chain input {
    type filter hook input priority -10; policy accept;
    tcp dport $port ip saddr @v4 accept
    tcp dport $port ip6 saddr @v6 accept
    tcp dport $port drop
  }
}
EOF
echo "cloudflare-only: port $port for Cloudflare's $(tr ',' '\n' <<<"$v4,$v6" | wc -l | tr -d ' ') ranges only"
