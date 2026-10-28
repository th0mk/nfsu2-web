#!/bin/bash
# The compare rules of tests/mworacle.c on the cases that made them, each
# with a mutation that must still be a DIFF (L-015: a rule that explains a
# difference must not explain a wrong result away).
#   tests/mworacle_rules.sh          (about 5 minutes; needs what tests/mworacle.sh needs)
#   QUICK=1 tests/mworacle_rules.sh  without the long re-runs (about 1 minute)
# 1. read back: 0x7D8CC0 (the C runtime's FDIV workaround) cases 71, 72, 148
#    store an 80-bit value no double holds and load its bytes into eax; the
#    read-back class takes them, and ORACLE_MUTATE=eax (the lifted eax ^ 1)
#    and ORACLE_MUTATE=f80 (a byte of the lifted side's fstp m80 flipped)
#    make each a DIFF.
# 2. a lifted side at its limit where the original returned: ORACLE_MUTATE=loop
#    on 0x7D8CC0 case 0 is a DIFF.
# 3. the original-loop re-run: 0x852570 case 3, where the lifted side returns
#    after about 5 s and Unicorn after about 70-85 s, is the same with the
#    default budget and a DIFF with ORACLE_LONG_ORIG_S=5.
# 4. a lifted side still at its limit after the 60 s re-run where the original
#    returns later: ORACLE_MUTATE=loop on 0x852570 case 3 is a DIFF with the
#    default budgets (the original gets the function's budget, with the loop
#    proof at first, which must not fire on a loop that ends).
# 5. the loop proof: 0x8267DC case 14 (shl edx,1 until edx >= ecx, edx 0
#    there) is the same state again after a few passes: a loop at once, not
#    after the function's budget.
# Exit 0 = every check as expected.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
D=out/mw/oracle; mkdir -p "$D"
echo '0x007d8cc0 1 vvvvvvvv vvvvvvv x -' > "$D/rules-7d8cc0.txt"
echo '0x00852570 1 ppvv vvvvvvv - -' > "$D/rules-852570.txt"
echo '0x008267dc 5 ppvv vvvvvvv - -' > "$D/rules-8267dc.txt"
fail=0; n=0
# check <name> <want: pass|diff> <grep pattern that must be in the output> <sample> <env...>
check(){
  local name="$1" want="$2" pat="$3" sample="$4"; shift 4
  local log="$D/rules-$n.log"; n=$((n + 1))
  set +e; env "$@" ORACLE_SAMPLE="$sample" tests/mworacle.sh 150 > "$log" 2>&1; local rc=$?; set -e
  local ok=1
  [ "$want" = pass ] && [ $rc -ne 0 ] && ok=0
  [ "$want" = diff ] && [ $rc -ne 1 ] && ok=0
  grep -q -- "$pat" "$log" || ok=0
  if [ $ok = 1 ]; then echo "  ok    $name"; else echo "  FAIL  $name (rc $rc, wanted $want and '$pat': $log)"; fail=$((fail + 1)); fi
}
for k in 71 72 148; do
  check "0x7D8CC0 case $k: read back as integers" pass "read back as integers 1," "$D/rules-7d8cc0.txt" ORACLE_FNS=7d8cc0 ORACLE_CASE=$k
  check "0x7D8CC0 case $k, eax ^ 1: DIFF" diff "DIFF 007d8cc0 case $k: eax 00000001" "$D/rules-7d8cc0.txt" ORACLE_FNS=7d8cc0 ORACLE_CASE=$k ORACLE_MUTATE=eax
  check "0x7D8CC0 case $k, a lifted fstp m80 byte flipped: DIFF" diff "DIFF 007d8cc0 case $k: .*the lifted fstp m80 bytes at 3000fc04 are neither image" \
    "$D/rules-7d8cc0.txt" ORACLE_FNS=7d8cc0 ORACLE_CASE=$k ORACLE_MUTATE=f80
done
check "0x7D8CC0 case 0, lifted side at its limit: DIFF" diff "DIFF 007d8cc0 case 0: outcome: lifted loop at 00000000, original ret" \
  "$D/rules-7d8cc0.txt" ORACLE_FNS=7d8cc0 ORACLE_CASE=0 ORACLE_MUTATE=loop
check "0x852570 case 3, original-loop budget 5 s: DIFF" diff "DIFF 00852570 case 3: outcome: lifted ret at fffe0000, original loop" \
  "$D/rules-852570.txt" ORACLE_FNS=852570 ORACLE_CASE=3 ORACLE_LONG_ORIG_S=5
if [ -z "${QUICK:-}" ]; then
  check "0x852570 case 3, the default budget: the same" pass "original ret after" \
    "$D/rules-852570.txt" ORACLE_FNS=852570 ORACLE_CASE=3
  check "0x852570 case 3, lifted side still at its limit, the original returns later: DIFF" diff \
    "DIFF 00852570 case 3: outcome: lifted loop at 00000000, original ret" "$D/rules-852570.txt" ORACLE_FNS=852570 ORACLE_CASE=3 ORACLE_MUTATE=loop
  check "0x8267DC case 14, both sides at their limit: a proven loop" pass "original loop after .* (proven: the same state again" \
    "$D/rules-8267dc.txt" ORACLE_FNS=8267dc ORACLE_CASE=14
fi
echo "mworacle_rules: $((n - fail)) of $n as expected"
[ $fail = 0 ]
