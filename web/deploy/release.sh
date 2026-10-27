#!/usr/bin/env bash
# A new version on a Docker VPS (docs/web/VPS.md, /srv/nfsu2) in one command, from your own machine: the game module
# of a commit, and the image of that commit (or a later one), then the server restarted and checked.
#
#   web/deploy/release.sh root@<vps> <folder>              the module in <folder> (nfsu2.wasm, nfsu2.json), and the
#                                                          image of its commit (sha-<rev> in nfsu2.json)
#   web/deploy/release.sh root@<vps> <folder> <commit>     the image of another commit, e.g. a later one that changed
#                                                          only the page or the server (the module stays the same)
#   web/deploy/release.sh root@<vps> --undo                back to the module and image before the last release
#   -n first: only say what it would do
#
# What it does: checks that the folder holds both files of one commit and that the registry has the image's tag;
# copies the files to <dir>/module.new; then on the VPS pulls the image first (the running server reads its module
# from disk at every request, so the new one goes in only once its image is there: a pull that fails leaves the site
# as it was, the files waiting in module.new), keeps the running module and .env (module.prev, .env.prev, for
# --undo; a try again of the same release keeps the ones before it), moves the new files into module/ (a rename
# each: no half-written file is ever served), makes them readable for the container (644), sets NFSU2_IMAGE in .env,
# restarts only the server at once (`docker compose up -d server`: cloudflared and the rest stay up), and waits
# until it is healthy with the new module.  Everyone in the game is cut off for a few seconds and reconnects; pages
# notice the new module by its ETag.  Last, module/release.json names the module's commit and the running image's,
# for the forum bot (discord-bot/repo.mjs): after a release of only the page or the server, the image's commit is
# what is live, though nfsu2.json still names the module's.  A try again of a release whose server is up already
# restarts nothing and only writes that file.
#
#   NFSU2_DIR    the folder on the VPS (default /srv/nfsu2)
#   NFSU2_REPO   the image without its tag (default ghcr.io/th0mk/nfsu2-web-rs)
set -euo pipefail

dry=0
[ "${1:-}" = -n ] && { dry=1; shift; }
vps="${1:-}"; arg="${2:-}"
dir="${NFSU2_DIR:-/srv/nfsu2}"
repo="${NFSU2_REPO:-ghcr.io/th0mk/nfsu2-web-rs}"
die(){ echo "release: $*" >&2; exit 1; }
[ -n "$vps" ] && [ -n "$arg" ] || die "usage: release.sh [-n] user@vps <module folder> [commit] | release.sh [-n] user@vps --undo"

# the part that runs on the VPS: $1 what (release or undo), $2 the image, $3 the module's commit
remote=$(cat <<'REMOTE'
set -euo pipefail
what=$1 img=$2 rev=$3
cd "$NFSU2_DIR"
say(){ echo "  [vps] $*"; }
if [ "$what" = undo ]; then
  [ -f module.prev/nfsu2.wasm ] && [ -f module.prev/nfsu2.json ] && [ -f .env.prev ] || { echo "release: no earlier release kept here" >&2; exit 1; }
  mkdir -p module.new
  cp -p module.prev/nfsu2.wasm module.prev/nfsu2.json module.new/
  rev=$(grep -o '"rev":"[^"]*"' module.new/nfsu2.json | cut -d'"' -f4)
  img=$(grep '^NFSU2_IMAGE=' .env.prev | cut -d= -f2- || true)
fi
[ -f module.new/nfsu2.wasm ] && [ -f module.new/nfsu2.json ] || { echo "release: no module in $NFSU2_DIR/module.new" >&2; exit 1; }
say "image: ${img:-as compose.yml says}"
NFSU2_IMAGE="$img" docker compose pull -q server
if [ "$what" = release ]; then
  # the module and image of this release in place already (a try that stopped after the move): the ones kept stay
  if cmp -s module/nfsu2.json module.new/nfsu2.json && [ "$(grep '^NFSU2_IMAGE=' .env | cut -d= -f2- || true)" = "$img" ]; then
    say "this release again: the module and .env of before it stay kept"
  else
    rm -rf module.prev && mkdir module.prev
    cp -p module/nfsu2.wasm module/nfsu2.json module.prev/ 2>/dev/null || true
    cp -p .env .env.prev
  fi
fi
chmod 644 module.new/nfsu2.wasm module.new/nfsu2.json
mv -f module.new/nfsu2.wasm module/nfsu2.wasm
mv -f module.new/nfsu2.json module/nfsu2.json
rmdir module.new
say "module: $(grep -o '"rev":"[^"]*"' module/nfsu2.json)"
if [ -n "$img" ]; then
  if grep -q '^NFSU2_IMAGE=' .env; then sed -i "s#^NFSU2_IMAGE=.*#NFSU2_IMAGE=$img#" .env; else echo "NFSU2_IMAGE=$img" >> .env; fi
else
  sed -i '/^NFSU2_IMAGE=/d' .env
fi
docker compose up -d server
say "waiting for the server to be healthy..."
for i in $(seq 1 90); do
  st=$(docker inspect --format '{{.State.Health.Status}}' "$(docker compose ps -q server)" 2>/dev/null || echo starting)
  [ "$st" = healthy ] && break
  [ "$st" = unhealthy ] && { echo "release: the server is unhealthy: docker compose logs server" >&2; exit 1; }
  sleep 2
done
[ "$st" = healthy ] || { echo "release: not healthy after 3 minutes: docker compose logs server" >&2; exit 1; }
label=$(docker inspect --format '{{index .Config.Labels "org.opencontainers.image.revision"}}' "$(docker compose ps -q server)" 2>/dev/null || true)
say "healthy: module $rev, image ${label:0:7}"
# what is live, for the forum bot: the image's commit from its label, else from its sha- tag
image=${label:0:7}
[ -n "$image" ] || image=$(echo "$img" | sed -n 's/.*:sha-\([0-9a-f]*\)$/\1/p')
if [ -n "$image" ]; then
  printf '{"module":"%s","image":"%s"}\n' "$rev" "$image" > module/release.json.new
  chmod 644 module/release.json.new
  mv -f module/release.json.new module/release.json
else
  rm -f module/release.json
fi
REMOTE
)

if [ "$arg" = --undo ]; then
  echo "release: $vps:$dir back to the module and image of before the last release"
  [ $dry = 1 ] && exit 0
  ssh "$vps" "NFSU2_DIR='$dir' bash -s -- undo '' ''" <<<"$remote"
  exit 0
fi

# the module: both files, one commit, not from a checkout with changes
src="$arg"
# never a build with the D3DX oracle linked: web/build.sh writes "oracle" into the json of such a build only.  Every
# json of the folder given, before the names: another game's module is named after it (web/build.sh writes Most
# Wanted's to out/web/mw/nfsmw.json).  Not its subfolders: only nfsu2.wasm and nfsu2.json are uploaded
for j in "$src"/*.json; do
  [ -f "$j" ] || continue
  ! grep -qE '"oracle"[[:space:]]*:' "$j" \
    || die "$j has \"oracle\": a dev-only module with the D3DX oracle (Microsoft's d3dx9_26.dll, lifted) linked, never released; build one without it (D3DX_ORACLE=0)"
done
[ -f "$src/nfsu2.wasm" ] && [ -f "$src/nfsu2.json" ] || die "$src needs nfsu2.wasm and nfsu2.json"
rev=$(grep -o '"rev":"[^"]*"' "$src/nfsu2.json" | cut -d'"' -f4)
size=$(grep -o '"size":[0-9]*' "$src/nfsu2.json" | cut -d: -f2)
[ -n "$rev" ] || die "$src/nfsu2.json has no rev"
case "$rev" in *+*) die "the module was built from a checkout with changes ($rev): build it from a commit";; esac
have=$(wc -c < "$src/nfsu2.wasm" | tr -d ' ')
[ "$size" = "$have" ] || die "nfsu2.json says $size bytes, nfsu2.wasm has $have: not one build"

# the image: its tag in the registry (a commit's image is missing when CI cancelled it for a newer push)
commit="${3:-$rev}"
# another commit's image: one after the module's, as the forum bot will take it (release.json)
if [ "${commit:0:7}" != "${rev:0:7}" ] && git cat-file -e "$commit^{commit}" 2>/dev/null && git cat-file -e "$rev^{commit}" 2>/dev/null; then
  git merge-base --is-ancestor "$rev" "$commit" || die "$commit does not come after the module's commit $rev"
fi
img="$repo:sha-${commit:0:7}"
reg="${repo%%/*}" path="${repo#*/}"
tok=$(curl -fsS "https://$reg/token?scope=repository:$path:pull" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p') || die "the registry $reg did not answer"
code=$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $tok" \
  -H 'Accept: application/vnd.oci.image.index.v1+json, application/vnd.docker.distribution.manifest.list.v2+json, application/vnd.docker.distribution.manifest.v2+json' \
  "https://$reg/v2/$path/manifests/sha-${commit:0:7}")
[ "$code" = 200 ] || die "no image $img (HTTP $code): CI may have cancelled it for a newer push; pass that newer commit if it changed only the page or the server"

echo "release: module $rev ($have bytes) and image $img to $vps:$dir"
[ $dry = 1 ] && exit 0
ssh "$vps" "mkdir -p '$dir/module.new'"
scp -q "$src/nfsu2.wasm" "$src/nfsu2.json" "$vps:$dir/module.new/"
ssh "$vps" "NFSU2_DIR='$dir' bash -s -- release '$img' '$rev'" <<<"$remote"
echo "release: done. Undo: web/deploy/release.sh $vps --undo"
