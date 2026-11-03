#!/bin/bash
# Disassemble sub_00583100 from the linked Switch ELF (inside docker or with aarch64 objdump).
set -e
ROOT="${ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}"
ELF=$ROOT/out/switch/nfsu2.elf
OBJDUMP=aarch64-none-elf-objdump
if ! command -v $OBJDUMP >/dev/null 2>&1; then
  # run via docker
  MSYS_NO_PATHCONV=1 docker run --rm -v "$ROOT:/src" -w /src devkitpro/devkita64 \
    aarch64-none-elf-objdump -d out/switch/nfsu2.elf > /tmp/nfsu2.dis
else
  $OBJDUMP -d "$ELF" > /tmp/nfsu2.dis
fi
awk '
  /<sub_00583100>:/{p=1; print; next}
  p && /^[0-9a-f]+ </{exit}
  p{print; n++; if(n>60) exit}
' /tmp/nfsu2.dis
echo "---"
echo "adrp/ldr of g_mem-like in function:"
awk '
  /<sub_00583100>:/{p=1; next}
  p && /^[0-9a-f]+ </{exit}
  p && /adrp/{a++}
  p && /ldr[ \t].*g_mem/{l++}
  END{print "adrp=" a+0}
' /tmp/nfsu2.dis
grep -n "g_mem" /tmp/nfsu2.dis | head -5 || true
# count adrp inside sub_00583100
python3 - <<'PY'
import re
text=open('/tmp/nfsu2.dis',errors='replace').read().splitlines()
# find function
start=None
for i,l in enumerate(text):
    if '<sub_00583100>:' in l:
        start=i; break
if start is None:
    print('fn not found'); raise SystemExit
lines=[]
for l in text[start+1:]:
    if re.match(r'^[0-9a-f]+ <', l): break
    lines.append(l)
adrp=sum(1 for l in lines if '\tadrp\t' in l or ' adrp ' in l)
ldr=sum(1 for l in lines if re.search(r'\tldr\t', l))
print(f'sub_00583100: lines={len(lines)} adrp={adrp} ldr={ldr}')
# show first 40
print('\n'.join(lines[:45]))
PY
