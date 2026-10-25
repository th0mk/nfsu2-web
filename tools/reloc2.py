#!/usr/bin/env python3
"""Parse by hand, from the unnamed section, the relocation table whose directory entry was zeroed."""
import sys, os, struct, collections
import pefile
pe = pefile.PE(sys.argv[1]); IB = pe.OPTIONAL_HEADER.ImageBase
secs=[]
for s in pe.sections:
    nm=s.Name.rstrip(b'\x00').decode('latin1').strip() or '<none>'
    secs.append((nm, IB+s.VirtualAddress, s.Misc_VirtualSize, s.get_data()))
blob = next(d for nm,va,vs,d in secs if nm=='<none>')

off=0; tot=0; types=collections.Counter(); addrs=[]; blocks=0; bad=0
while off+8 <= len(blob):
    page, size = struct.unpack_from('<II', blob, off)
    if size==0: break
    if size<8 or off+size > len(blob)+8:
        bad+=1; break
    n=(min(size,len(blob)-off)-8)//2
    for i in range(n):
        (w,)=struct.unpack_from('<H', blob, off+8+i*2)
        t=w>>12; o=w&0xfff
        types[t]+=1; tot+=1
        if t==3: addrs.append(IB+page+o)
    blocks+=1; off+=size
print(f"blocks parsed: {blocks}   entries in total: {tot}   bad: {bad}")
for t,c in sorted(types.items()): print(f"  type {t}: {c}")

def sec_of(a):
    for nm,va,vs,d in secs:
        if va<=a<va+vs: return nm
    return '<outside>'
print("\nsections holding the absolute-address fixups:")
for k,v in collections.Counter(map(sec_of,addrs)).most_common(): print(f"  {k:10s} {v}")

# Check: do the 4-byte values at these locations really point into the image?
def rd32(a):
    for nm,va,vs,d in secs:
        if va<=a<va+len(d)-3: return struct.unpack_from('<I',d,a-va)[0]
    return None
IMG_LO, IMG_HI = IB, IB+pe.OPTIONAL_HEADER.SizeOfImage
ok=sum(1 for a in addrs if (v:=rd32(a)) is not None and IMG_LO<=v<IMG_HI)
print(f"\nCHECK: the values at {ok}/{len(addrs)} locations point into the image ({IMG_LO:#x}-{IMG_HI:#x}) = {100.0*ok/max(len(addrs),1):.2f}%")
# OUT_DIR (gen.sh: the game's, games/<id>/game.conf): where relocs.txt goes
od=os.environ.get('OUT_DIR','out'); out=os.path.join(od,'relocs.txt'); os.makedirs(od, exist_ok=True)
open(out,'w').write('\n'.join(f'{a:08x}' for a in sorted(addrs)))
print(f"-> {out} ({len(addrs)} addresses)")
