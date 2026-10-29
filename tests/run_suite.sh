#!/bin/bash
# Regression suite: runs the scripted scenarios and checks each log.
# "keyboard" drives everything with real NSEvents (runtime/keyboard_mac.m):
# title, profile, a keyboard rebind on the Controls page, a race.
#   tests/run_suite.sh <outdir> [scenario ...]
# A scenario passes when the run exits 0 via the script's "quit", with no
# fault ("FAULT IN GUEST", ...), no timeout ("TIME LIMIT REACHED"), no trap, no unresolved
# indirect call, no failed "expect" check of the input script, no
# "[net] BLOCKED" line (an attempt to reach a network beyond the test's
# loopback / virtual LAN), (where given) the expected marker in the log, and
# (where present) a passing tests/checks/<name>.sh LOG (its output goes to
# <outdir>/<name>/check.txt).  The suite runs with NFSU2_NET=off
# (tools/autorun.sh) unless a scenario sets it.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:?usage: tests/run_suite.sh <outdir> [scenario...]}"; shift
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"
SEED="${NFSU2_REGFILE_SEED:-$ROOT/tests/web/seed/registry_perf4_native.txt}"
# name : script : expected log marker : extra env
# (careerbot: the test-only career bot, runtime/career.c, ends the run itself)
ALL="
quickrace:quickrace.txt:waitmem [0x008654a4]==6 satisfied:
sprint:qr_mode1.txt:waitmem [0x008654a4]==6 satisfied:
drag:qr_mode2.txt:waitmem [0x008654a4]==6 satisfied:
drift:qr_mode3.txt:waitmem [0x008654a4]==6 satisfied:
driftscore:drift_score.txt:[expect] PASS:NFSU2_FIXED_DT=120 ALARM=600
streetx:qr_mode4.txt:waitmem [0x008654a4]==6 satisfied:
url:qr_mode5.txt:waitmem [0x008654a4]==6 satisfied:
career:career_prologue.txt:waitfng HUD_SingleRace.fng satisfied:
customize:mainmenu_item3.txt:screen UI_GenericParts_Browser.fng:
movies:career_movies.txt:tapuntil UI_StartCareer.fng satisfied:NOMOVIES=0
options:options_display.txt:[D3D] Reset:
hornvolume:options_horn.txt:[horn] volume 85 %:
hornpause:pause_horn.txt:parts as SLIDER_BASE_8 SLIDER_KNOB_8 SLIDER_HANDLE_8 SLIDER_BEG_8 SLIDER_END_8:
tone:options_tone.txt:[tone] white level 70 %:
tonepause:pause_tone.txt:[tone] white level 90 %:
powerunits:options_units.txt:[units] power in bhp:
eucars:qr_eucars.txt:[expect] PASS:
keyboard:kb_rebind_save.txt:waitmem [0x008654a4]==6 satisfied:
careerbot:career_bot.txt:segment done:NFSU2_CAREER_BOT_RACES=2 ALARM=600
lan_host:lan_host_short.txt:auth NAME 'SUITEHOST':NFSU2_NET=loopback NFSU2_NET_LOOPOFFSET=2101
"
# opt-in, run only when named: needs a career save from stage 2 on in
# NFSU2_SAVE_SEED (tests/scripts/career_outrun.txt)
OPT="
outrun:career_outrun.txt:player completion 6 (6 won:NFSU2_CAREER_RACEWIN=3
"
[ $# -gt 0 ] && ALL="$ALL$OPT"
pass=0; fail=0
while IFS= read -r line; do
  [ -z "$line" ] && continue
  IFS=: read -r name script marker extra <<< "$line"
  [ $# -gt 0 ] && ! printf '%s\n' "$@" | grep -qx "$name" && continue
  d="$OUT/$name"; rm -rf "$d"
  NFSU2_REGFILE_SEED="$SEED" "$ROOT/tools/autorun.sh" "$d" NFSU2_DISCORD=0 NOFOCUS=2 NFSU2_FNG_LOG=1 \
     NFSU2_STATUS_EVERY=1200 NFSU2_DUMP_SCALE=0.3 ALARM=400 \
     NFSU2_INPUT_SCRIPT="$ROOT/tests/scripts/$script" ${extra:+$extra} > "$d.run" 2>&1 < /dev/null
  rc=$?; log="$d/log.txt"; why=""
  [ $rc -eq 0 ] || why="$why exit=$rc"
  LC_ALL=C grep -aq "quit at guest frame" "$log" || why="$why no-quit"
  LC_ALL=C grep -aq "FAULT IN GUEST\|GUEST MEMORY FAULT\|HOST SEGV" "$log" && why="$why fault"
  LC_ALL=C grep -aq "TIME LIMIT REACHED" "$log" && why="$why timeout"
  LC_ALL=C grep -aq "^\[trap\]" "$log" && why="$why trap"
  LC_ALL=C grep -aq "UNRESOLVED INDIRECT" "$log" && why="$why unresolved-icall"
  LC_ALL=C grep -aq "^\[expect\] FAIL" "$log" && why="$why expect-failed"
  LC_ALL=C grep -aq "^\[net\] BLOCKED" "$log" && why="$why net-blocked"
  [ -n "$marker" ] && ! LC_ALL=C grep -aqF "$marker" "$log" && why="$why missing:'$marker'"
  chk="$ROOT/tests/checks/$name.sh"
  [ -x "$chk" ] && ! "$chk" "$log" > "$d/check.txt" 2>&1 && why="$why check-failed($(tail -1 "$d/check.txt"))"
  st=$(LC_ALL=C grep -a "^\[status\]" "$log" | tail -1 | sed 's/^\[status\] //')
  "$ROOT/.venv/bin/python" "$ROOT/tools/contact_sheet.py" "$d" 5 260 >/dev/null 2>&1
  if [ -z "$why" ]; then pass=$((pass+1)); echo "PASS $name  ($st)"; else fail=$((fail+1)); echo "FAIL $name:$why"; fi
done <<< "$ALL"
echo "suite: $pass passed, $fail failed  (contact sheets: $OUT/*/contact.png)"
[ $fail -eq 0 ]
