#!/bin/bash
# Count RIP-relative loads (g_mem) inside sub_00583100 for each object.
for tag in base gc gl; do
  f=/tmp/s107_${tag}.o
  [ -f "$f" ] || continue
  n=$(objdump -d "$f" | awk '
    /<sub_00583100>:/{p=1; next}
    p && /^[0-9a-f]+ </{exit}
    p && /mov[a-z]*[ \t].*%rip/{c++}
    END{print c+0}')
  sz=$(objdump -d "$f" | awk '
    /<sub_00583100>:/{p=1; next}
    p && /^[0-9a-f]+ </{exit}
    p{c++}
    END{print c+0}')
  echo "$tag: rip_loads=$n insn_lines=$sz"
done
