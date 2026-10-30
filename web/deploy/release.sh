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
#   --game mw first (after -n): NFS Most Wanted's box instead, with its own names: the folder /srv/nfsmw, whose
#   compose files are compose.mw.yml's (web/deploy/compose.mw.yml), and the module nfsmw.wasm and nfsmw.json
#   (GAME=mw web/build.sh: out/web/mw); the same image (it holds both games' pages)
#     web/deploy/release.sh --game mw root@<mw-vps> out/web/mw
#   It refuses a module json of another game ("game" in it; one with none is Underground 2's, as web/build.sh wrote
#   them before Most Wanted), a folder on the VPS that is not the game's (Most Wanted's only /srv/nfsmw; Underground
#   2's never /srv/nfsmw) and, on the VPS, a folder whose .env does not run the game's compose files, before it
#   changes anything there.
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
#   NFSU2_DIR    the folder on the VPS (default /srv/nfsu2; with --game mw /srv/nfsmw, and only that)
#   NFSU2_REPO   the image without its tag (default ghcr.io/th0mk/nfsu2-web-rs)
set -euo pipefail

dry=0
[ "${1:-}" = -n ] && { dry=1; shift; }
game=ug2
if [ "${1:-}" = --game ]; then game="${2:-}"; shift $(( $# < 2 ? $# : 2 )); fi
die(){ echo "release: $*" >&2; exit 1; }
# each game's names (one image holds both games' pages): the folder on the VPS, the module's file names
case "$game" in
  ug2) gdir=/srv/nfsu2 name=nfsu2 ;;
  mw) gdir=/srv/nfsmw name=nfsmw ;;
  *) die "--game: ug2 or mw" ;;
esac
vps="${1:-}"; arg="${2:-}"
dir="${NFSU2_DIR:-$gdir}"
repo="${NFSU2_REPO:-ghcr.io/th0mk/nfsu2-web-rs}"
[ -n "$vps" ] && [ -n "$arg" ] || die "usage: release.sh [-n] [--game mw] user@vps <module folder> [commit] | release.sh [-n] [--game mw] user@vps --undo"
# the folder on the VPS: Most Wanted's is /srv/nfsmw and no other, and no other game's is (one game's module in the
# other's folder is the mix-up this refuses: a box serves one game)
case "$game:${dir%/}" in
  mw:/srv/nfsmw) ;;
  mw:*) die "$dir is not Most Wanted's folder: --game mw releases to /srv/nfsmw only" ;;
  *:/srv/nfsmw) die "$dir is Most Wanted's folder: --game mw" ;;
esac

# the part that runs on the VPS: $1 what (release or undo), $2 the image, $3 the module's commit, $4 the game, $5 its
# module's file name.  First, the folder is the game's: its .env runs Most Wanted's compose file (compose.mw.yml) for
# mw, and never for ug2 (else nothing is pulled, kept or moved)
remote=$(cat <<'REMOTE'
set -euo pipefail
what=$1 img=$2 rev=$3 game=${4:-ug2} name=${5:-nfsu2}
cd "$NFSU2_DIR"
say(){ echo "  [vps] $*"; }
mwfiles=0
grep -q '^COMPOSE_FILE=.*compose\.mw\.yml' .env 2>/dev/null && mwfiles=1
if [ "$game" = mw ] && [ $mwfiles = 0 ]; then echo "release: $NFSU2_DIR is not Most Wanted's: its .env's COMPOSE_FILE has no compose.mw.yml" >&2; exit 1; fi
if [ "$game" != mw ] && [ $mwfiles = 1 ]; then echo "release: $NFSU2_DIR is Most Wanted's (compose.mw.yml in its .env): --game mw" >&2; exit 1; fi
if [ "$what" = undo ]; then
  [ -f module.prev/$name.wasm ] && [ -f module.prev/$name.json ] && [ -f .env.prev ] || { echo "release: no earlier release kept here" >&2; exit 1; }
  mkdir -p module.new
  cp -p module.prev/$name.wasm module.prev/$name.json module.new/
  rev=$(grep -o '"rev":"[^"]*"' module.new/$name.json | cut -d'"' -f4)
  img=$(grep '^NFSU2_IMAGE=' .env.prev | cut -d= -f2- || true)
fi
[ -f module.new/$name.wasm ] && [ -f module.new/$name.json ] || { echo "release: no module in $NFSU2_DIR/module.new" >&2; exit 1; }
say "image: ${img:-as compose.yml says}"
NFSU2_IMAGE="$img" docker compose pull -q server
if [ "$what" = release ]; then
  # the module and image of this release in place already (a try that stopped after the move): the ones kept stay
  if cmp -s module/$name.json module.new/$name.json && [ "$(grep '^NFSU2_IMAGE=' .env | cut -d= -f2- || true)" = "$img" ]; then
    say "this release again: the module and .env of before it stay kept"
  else
    rm -rf module.prev && mkdir module.prev
    cp -p module/$name.wasm module/$name.json module.prev/ 2>/dev/null || true
    cp -p .env .env.prev
  fi
fi
chmod 644 module.new/$name.wasm module.new/$name.json
mv -f module.new/$name.wasm module/$name.wasm
mv -f module.new/$name.json module/$name.json
rmdir module.new
say "module: $(grep -o '"rev":"[^"]*"' module/$name.json)"
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
  ssh "$vps" "NFSU2_DIR='$dir' bash -s -- undo '' '' '$game' '$name'" <<<"$remote"
  exit 0
fi

# the module: both files, one commit, not from a checkout with changes
src="$arg"
# never a build with the D3DX oracle linked: web/build.sh writes "oracle" into the json of such a build only.  Every
# json of the folder given, before the names: another game's module is named after it (web/build.sh writes Most
# Wanted's to out/web/mw/nfsmw.json).  Not its subfolders: only the game's two files are uploaded (nfsu2.wasm and
# nfsu2.json; with --game mw nfsmw.wasm and nfsmw.json)
for j in "$src"/*.json; do
  [ -f "$j" ] || continue
  ! grep -qE '"oracle"[[:space:]]*:' "$j" \
    || die "$j has \"oracle\": a dev-only module with the D3DX oracle (Microsoft's d3dx9_26.dll, lifted) linked, never released; build one without it (D3DX_ORACLE=0)"
done
[ -f "$src/$name.wasm" ] && [ -f "$src/$name.json" ] || die "$src needs $name.wasm and $name.json"
# the module's game (web/build.sh writes "game"; Underground 2's json had none before Most Wanted): the one --game says
mgame=$(grep -o '"game":"[^"]*"' "$src/$name.json" | cut -d'"' -f4 || true)
[ "${mgame:-ug2}" = "$game" ] || die "$src/$name.json is a module of ${mgame:-ug2}, not of $game (--game)"
rev=$(grep -o '"rev":"[^"]*"' "$src/$name.json" | cut -d'"' -f4)
size=$(grep -o '"size":[0-9]*' "$src/$name.json" | cut -d: -f2)
[ -n "$rev" ] || die "$src/$name.json has no rev"
case "$rev" in *+*) die "the module was built from a checkout with changes ($rev): build it from a commit";; esac
have=$(wc -c < "$src/$name.wasm" | tr -d ' ')
[ "$size" = "$have" ] || die "$name.json says $size bytes, $name.wasm has $have: not one build"

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
scp -q "$src/$name.wasm" "$src/$name.json" "$vps:$dir/module.new/"
ssh "$vps" "NFSU2_DIR='$dir' bash -s -- release '$img' '$rev' '$game' '$name'" <<<"$remote"
again=""; [ "$game" = ug2 ] || again="--game $game "
echo "release: done. Undo: web/deploy/release.sh $again$vps --undo"
