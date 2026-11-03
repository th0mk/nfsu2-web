#!/bin/bash
set -e
ROOT="${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}"
INC=$HOME/nfsu2-linux-build/inc
COMMON="-O2 -fno-strict-aliasing -fno-delete-null-pointer-checks -fsigned-char -D__SWITCH__ -DNFSU2_LINUX_HOST=1 -DNFS_ENHANCED=0 -w -include sys/socket.h -include $ROOT/runtime/switch/switch_plat.h -I$INC -I$ROOT/runtime/web/compat -I$ROOT/runtime -I$ROOT/gen/all"
SRC=$ROOT/gen/all/shard_0107.c
gcc $COMMON -c -o /tmp/s107_base.o "$SRC"
gcc $COMMON -DNFSU2_NX_GCONST -c -o /tmp/s107_gc.o "$SRC"
# Prototype: local restrict base via NFSU2_NX_GLOCAL (edit applied in cpu.h before this runs)
gcc $COMMON -DNFSU2_NX_GLOCAL -c -o /tmp/s107_gl.o "$SRC" 2>/tmp/s107_gl.err || { echo "GLOCAL compile failed:"; cat /tmp/s107_gl.err; }
echo "base g_mem refs: $(objdump -d /tmp/s107_base.o | grep -c 'g_mem' || true)"
echo "gconst g_mem refs: $(objdump -d /tmp/s107_gc.o | grep -c 'g_mem' || true)"
echo "gconst g_mem_c refs: $(objdump -d /tmp/s107_gc.o | grep -c 'g_mem_c' || true)"
if [ -f /tmp/s107_gl.o ]; then
  echo "glocal g_mem refs: $(objdump -d /tmp/s107_gl.o | grep -c 'g_mem' || true)"
fi
echo "===== BASE sub_00583100 ====="
objdump -d /tmp/s107_base.o | awk '/<sub_00583100>:/{p=1} p{print} /^$/{if(p){c++; if(c>0 && seen++) exit}}' | head -50
echo "===== GCONST sub_00583100 ====="
objdump -d /tmp/s107_gc.o | awk '/<sub_00583100>:/{p=1} p{print} /^$/{if(p){c++; if(c>0 && seen++) exit}}' | head -50
if [ -f /tmp/s107_gl.o ]; then
  echo "===== GLOCAL sub_00583100 ====="
  objdump -d /tmp/s107_gl.o | awk '/<sub_00583100>:/{p=1} p{print} /^$/{if(p){c++; if(c>0 && seen++) exit}}' | head -50
fi
