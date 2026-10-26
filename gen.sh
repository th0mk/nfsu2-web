#!/bin/bash
# Regenerate everything derived from SPEED2.EXE: relocs, lifted C, import args.
#   ./gen.sh [path/to/SPEED2.EXE]      (default: $NFSU2_DATA/SPEED2.EXE)
#   GAME=mw ./gen.sh path/to/speed.exe another game (tools/gameconf.sh): its
#                                      games/<id>/game.conf names the exe (one
#                                      whose SHA-256 differs is refused), the
#                                      output folders (GEN_DIR, OUT_DIR) and the
#                                      addresses the lifter needs
# The OVERRIDE list is derived from the runtime sources (every sub_X_orig the
# runtime references, plus the DX_WRAP(...) table in d3dx_hle.c), so it can
# never drift out of sync with runtime/overrides.c + runtime/d3dx_hle.c
# (another game's: its runtime/<id>/*.c, tools/genstamp.sh).
# When it has finished, gen/all/stamp.txt (and gen/server/stamp.txt) say what
# the lift was made from (tools/genstamp.sh): hashes of the override list,
# the hook list and the lifter, the exe, and the lift options (and the game,
# for another game than UG2); a build can then refuse a gen/all that is older
# than its runtime or another game's (web/build.sh does).
# A worktree's gen/all or gen/server that links to the shared one (or another
# game's GEN_DIR, or a folder on its way) is refused, as web/gen.sh refuses
# it: GEN_SHARED=1 regenerates the shared one on purpose (tools/gameconf.sh).
# The paths here are UG2's; another game's are under its GEN_DIR and OUT_DIR.
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"; cd "$ROOT"
. tools/gameconf.sh
G="$GEN_DIR"; O="$OUT_DIR"
EXE="${1:-${NFSU2_DATA:+$NFSU2_DATA/$GC_EXE_NAME}}"
[ -f "$EXE" ] || { echo "usage: ./gen.sh path/to/$GC_EXE_NAME"; exit 1; }
# before anything is written: a link to the shared gen, a refused exe, or a core
# file holding a game's override or hook marker (tools/genstamp.sh refuses it),
# leaves the last lift as it was
gameconf_nolink ./gen.sh
gameconf_files "$EXE"
STAMP_SOURCES="$(tools/genstamp.sh sources)"
PY="$ROOT/.venv/bin/python"
# no stamp until this run has finished; web/gen.sh's label is for its own runs only
mkdir -p "$G/all"
rm -f "$G/all/web-lift.txt"
echo "incomplete: gen.sh has not finished (or failed)" > "$G/all/stamp.txt"
OV=$(tools/genstamp.sh overrides)
echo "OVERRIDE ($(echo "$OV" | tr ',' '\n' | grep -c .)): $OV"
# Instruction hooks (guest_hook before the instruction): every HOOK_SITE(0x...)
# in the runtime sources, plus any extra HOOKS=... given for debugging.
HK=$( { tools/genstamp.sh hooks | tr ',' '\n'; echo "$HOOKS" | tr ',' '\n'; } | grep -E '^0x' | sort -u | paste -sd, -)
echo "HOOKS ($(echo "$HK" | tr ',' '\n' | grep -c .)): $HK"
# x87 precision control (tools/lift.py): on by
# default, so the game thread computes at PC=24 like on Windows; X87PC=0
# lifts plain doubles (the code before this option).
export X87PC="${X87PC:-1}"
echo "X87PC=$X87PC"
"$PY" tools/reloc2.py "$EXE"
OVERRIDE="$OV" HOOKS="$HK" "$PY" tools/genall.py "$EXE" "${NSHARD:-128}"
# STRICT=1: every import's stdcall argument count from argcount.py's KNOWN table, none guessed
STRICT=1 "$PY" tools/argcount.py "$EXE" > "$O/argcount.log"
echo "gen OK -> $G/all ($(ls "$G"/all/shard_*.c | wc -l | tr -d ' ') shards)"

# Second guest module: EA's LAN server, server.dll next to SPEED2.EXE, lifted
# at the game's fixed guest base SRV_BASE (UG2: 0x00A00000; runtime/module.c
# maps the original file there and checks its SHA-256 against the one recorded
# in gen/server).  The exe's output above does not depend on it.  Without
# server.dll a stub module is generated: the build links, and hosting a LAN
# game reports "unavailable".
SRV="$(dirname "$EXE")/server.dll"
mkdir -p "$G/server" && find "$G/server/" -mindepth 1 -delete   # empties it; with GEN_SHARED=1 a link to the shared one stays a link
if [ -f "$SRV" ]; then
  "$PY" tools/prepare_module.py "$SRV" "$GC_SRV_BASE" "$O/server" "$G/server" srv_
  GENOUT="$G/server" RELOCS="$O/server/relocs.txt" SEEDS="$O/server/seeds.txt" SYMPFX=srv_ \
    OVERRIDE="$OV" HOOKS="$HK" "$PY" tools/genall.py "$O/server/image.dll" "${NSHARD_SRV:-8}" > "$O/server/genall.log"
  grep -E "functions  *:|traps \(|truncated|MISSING" "$O/server/genall.log" | sed 's/^/  server.dll: /'
  RELOCS="$O/server/relocs.txt" IMPOUT="$G/server/impargs.c" SYMPFX=srv_ STRICT=1 IMPDLL="$SRV" \
    "$PY" tools/argcount.py "$O/server/image.dll" > "$O/server/argcount.log"
  echo "gen OK -> $G/server ($(ls "$G"/server/shard_*.c | wc -l | tr -d ' ') shards)"
else
  mkdir -p "$G/server"
  printf '#include "module.h"\nconst ModInfo g_srv_modinfo = { "server.dll", "", 0x%08xu, 0, 0, 0, 0, 0, 0 };\n' "$GC_SRV_BASE" > "$G/server/modinfo.c"
  printf '#include "cpu.h"\ntypedef void(*guest_fn)(CPU*);\nconst uint32_t g_srv_fn_count = 0;\nconst uint32_t g_srv_fn_addr[1] = {0};\nconst guest_fn g_srv_fn_ptr[1] = {0};\n' > "$G/server/fntab.c"
  printf '#include <stdint.h>\ntypedef struct { uint32_t iat; const char*dll; const char*name; int nargs; } ImpEnt;\nconst uint32_t g_srv_imp_count = 0;\nconst ImpEnt g_srv_imp[1] = {{0}};\n' > "$G/server/impargs.c"
  printf '#ifndef DECLS_H\n#define DECLS_H\n#include "cpu.h"\n#endif\n' > "$G/server/decls.h"
  echo "gen: no $SRV -> stub $G/server (LAN hosting unavailable in this build)"
fi

# what this lift was made from (tools/genstamp.sh; the source lines are compared by web/build.sh);
# a game's own lifter switch (game.conf LOCKSTRIP) is named only when it is on
h16(){ shasum -a 256 | cut -c1-16; }
STAMP="$(echo "# gen.sh $(date -u +%Y-%m-%dT%H:%M:%SZ): what $G/all and $G/server were lifted from"
  echo "$STAMP_SOURCES"
  echo "exe $(h16 < "$EXE")$( [ -f "$SRV" ] && echo " server.dll $(h16 < "$SRV")")"
  echo "options X87PC=$X87PC X87OPT=${X87OPT:-} JTSWITCH=${JTSWITCH:-} LREGS=${LREGS:-} FTOL2INL=${FTOL2INL:-} FLAGABI=$( [ -z "${FLAGABI:-}" ] && echo - || { [ -f "$FLAGABI" ] && h16 < "$FLAGABI" || echo missing; } )" \
       "FLAGOPT=${FLAGOPT:-} RETEIP=${RETEIP:-} INLINE=${INLINE:-} INLINE_LIST=$( [ -z "${INLINE_LIST:-}" ] && echo - || basename "$INLINE_LIST") NOCLOSURE=${NOCLOSURE:-} COMMENTS=${COMMENTS:-}" \
       "WATCH=${WATCH:-} COVERAGE=${COVERAGE:-} NSHARD=${NSHARD:-128}/${NSHARD_SRV:-8} HOOKS=${HOOKS:-}$( [ "$LOCKSTRIP" != 1 ] || echo " LOCKSTRIP=1")")"
echo "$STAMP" > "$G/server/stamp.txt"
echo "$STAMP" > "$G/all/stamp.txt"
