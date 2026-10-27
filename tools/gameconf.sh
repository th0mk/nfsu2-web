# The game the lift pipeline works on; sourced by gen.sh and web/gen.sh from
# the checkout's root.
#   GAME=<id> (default ug2) reads games/<id>/game.conf: plain KEY=VALUE lines
#   (no quotes, no spaces, no comment after a value; # starts a comment line).
#   Each key is read into GC_<KEY>, so the caller's own environment stays as it
#   is (web/gen.sh lets FTOL2INL and INLINE_LIST given there win over the
#   game's).  The tools get the game's paths and addresses from the exports at
#   the end: GEN_DIR (gen/all, gen/server), OUT_DIR (relocs, flag table, logs),
#   FTOL2_VA, LOCKSTRIP, X87ENV and RETPAST_LIST (tools/lift.py).  A game with
#   a D3DX_BASE (its d3dx9 DLL lifted as the dev-only D3DX oracle) names the
#   DLL's D3DX_SHA256 and D3DX_CODE (the code in its .text) too, which gen.sh
#   needs when D3DX_DLL asks it to lift the DLL, and may name D3DX_OPTNONE (its
#   functions compiled without optimisation, tools/genall.py OPTNONE).
#   gameconf_files <exe>   stops unless <exe>, and server.dll next to it if
#                          there is one, are the files game.conf names (SHA-256)
#   gameconf_d3dx          stops when D3DX_DLL is set and the game has no D3DX
#                          oracle, game.conf lacks D3DX_SHA256 or D3DX_CODE, or
#                          the file is not the one D3DX_SHA256 names
#   gameconf_sha <file> <sha256> <what>  stops unless <file> has that SHA-256
#   gameconf_nolink <script>  stops when GEN_DIR, a folder on its way there,
#                          GEN_DIR/all, GEN_DIR/server or GEN_DIR/d3dx is a
#                          link (a worktree's link to the shared gen, which
#                          other worktrees build against), unless GEN_SHARED=1:
#                          gen.sh and web/gen.sh run it before they write
#                          anything
GAME="${GAME:-ug2}"
case "$GAME" in ''|*[!a-z0-9]*) echo "GAME=$GAME: not a game id (games/<id>/game.conf)"; exit 1 ;; esac
GAME_CONF="games/$GAME/game.conf"
[ -f "$GAME_CONF" ] || { echo "GAME=$GAME: no $GAME_CONF"; exit 1; }
while IFS= read -r _l || [ -n "$_l" ]; do
  case "$_l" in ''|'#'*) continue ;; esac
  _k="${_l%%=*}"
  case "$_k" in ''|*[!A-Z0-9_]*) echo "$GAME_CONF: not a KEY=VALUE line: $_l"; exit 1 ;; esac
  [ "$_k" != "$_l" ] || { echo "$GAME_CONF: not a KEY=VALUE line: $_l"; exit 1; }
  eval "GC_$_k=\${_l#*=}"
done < "$GAME_CONF"
for _k in EXE_NAME EXE_SHA256 FTOL2_VA SRV_BASE SRV_SHA256 GEN_DIR OUT_DIR FTOL2INL INLINE_LIST LOCKSTRIP X87ENV RETPAST_LIST; do
  eval "_v=\${GC_$_k+set}"
  [ -n "$_v" ] || { echo "$GAME_CONF: no $_k="; exit 1; }
done
for _v in "$GC_GEN_DIR" "$GC_OUT_DIR"; do
  case "$_v" in ''|/*|*..*|*[!A-Za-z0-9_./-]*) echo "$GAME_CONF: $_v: GEN_DIR and OUT_DIR are folders under the checkout"; exit 1 ;; esac
done
unset _l _k _v

# gameconf_sha <file> <sha256> <what>: stop unless <file> has that SHA-256
gameconf_sha(){
  local got; got=$(shasum -a 256 < "$1" | cut -d' ' -f1)
  [ "$got" = "$(echo "$2" | tr 'A-F' 'a-f')" ] || {
    echo "$1: not $GAME's $3 (SHA-256 $got; $GAME_CONF wants $2)"; exit 1; }
}
gameconf_files(){
  gameconf_sha "$1" "$GC_EXE_SHA256" "$GC_EXE_NAME"
  if [ -f "$(dirname "$1")/server.dll" ]; then gameconf_sha "$(dirname "$1")/server.dll" "$GC_SRV_SHA256" server.dll; fi
}
gameconf_d3dx(){
  [ -n "${D3DX_DLL:-}" ] || return 0
  [ -n "${GC_D3DX_BASE:-}" ] || { echo "D3DX_DLL: $GAME has no D3DX oracle ($GAME_CONF has no D3DX_BASE)"; exit 1; }
  [ -n "${GC_D3DX_SHA256:-}" ] && [ -n "${GC_D3DX_CODE:-}" ] || { echo "$GAME_CONF: D3DX_BASE needs D3DX_SHA256 and D3DX_CODE"; exit 1; }
  [ -f "$D3DX_DLL" ] || { echo "D3DX_DLL=$D3DX_DLL: no such file"; exit 1; }
  gameconf_sha "$D3DX_DLL" "$GC_D3DX_SHA256" "D3DX DLL"
}
gameconf_nolink(){
  local d p= dirs=
  for d in $(echo "$GC_GEN_DIR" | tr '/' ' '); do p="${p:+$p/}$d"; dirs="$dirs $p"; done
  for d in $dirs "$GC_GEN_DIR/all" "$GC_GEN_DIR/server" "$GC_GEN_DIR/d3dx"; do
    if [ -L "$d" ] && [ "${GEN_SHARED:-}" != 1 ]; then
      echo "$1: $d is a link to $(readlink "$d"), which other worktrees build against;"
      echo "  rm $d && mkdir $d to lift into this worktree, or GEN_SHARED=1 $( [ "$GAME" = ug2 ] || echo "GAME=$GAME ")$1 to regenerate the shared one"
      exit 1
    fi
  done
}

export GAME GEN_DIR="$GC_GEN_DIR" OUT_DIR="$GC_OUT_DIR" FTOL2_VA="$GC_FTOL2_VA" LOCKSTRIP="$GC_LOCKSTRIP" X87ENV="$GC_X87ENV" RETPAST_LIST="$GC_RETPAST_LIST"
