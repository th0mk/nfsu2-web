#!/bin/bash
set -e
ROOT="${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}"
cd "$ROOT"
for s in sub_00583100 sub_005875c0 sub_0057cd10 sub_0058f020 sub_00594220 sub_00578060 sub_004a4480; do
  echo "===== $s ====="
  # which shard defines it
  f=$(grep -l "^void $s\|^REG_FN.*$s\|$s(CPU" gen/all/shard_*.c 2>/dev/null | head -1 || true)
  if [ -z "$f" ]; then
    f=$(grep -l "$s" gen/all/shard_*.c 2>/dev/null | head -1 || true)
  fi
  echo "file: $f"
  if [ -n "$f" ]; then
    # show first ~40 lines of the function body
    awk -v fn="$s" '
      $0 ~ "void "fn"\\(CPU" || $0 ~ "^"fn"\\(CPU" {p=1}
      p {print; n++; if(n>50) exit}
    ' "$f"
  fi
  echo
done
# also try objdump of the binary for the hottest
cd ~/nfsu2-linux-build
echo "===== objdump sub_00583100 ====="
objdump -d --disassemble=sub_00583100 nfsu2_bench | head -80
